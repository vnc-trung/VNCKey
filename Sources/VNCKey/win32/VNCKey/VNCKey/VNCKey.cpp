/*----------------------------------------------------------
OpenKey - The Cross platform Open source Vietnamese Keyboard application.

Copyright (C) 2019 Mai Vu Tuyen
Contact: maivutuyen.91@gmail.com
Github: https://github.com/tuyenvm/OpenKey
Fanpage: https://www.facebook.com/OpenKeyVN

This file is belong to the OpenKey project, Win32 version
which is released under GPL license.
You can fork, modify, improve this program. If you
redistribute your new version, it MUST be open source.
-----------------------------------------------------------*/
#include "stdafx.h"
#include "AppDelegate.h"
#include "TerminalDetector.h"

#pragma comment(lib, "imm32")
#define IMC_GETOPENSTATUS 0x0005

#define MASK_SHIFT				0x01
#define MASK_CONTROL			0x02
#define MASK_ALT				0x04
#define MASK_CAPITAL			0x08
#define MASK_NUMLOCK			0x10
#define MASK_WIN				0x20
#define MASK_SCROLL				0x40

#define OTHER_CONTROL_KEY (_flag & MASK_ALT) || (_flag & MASK_CONTROL)
#define DYNA_DATA(macro, pos) (macro ? pData->macroData[pos] : pData->charData[pos])
#define EMPTY_HOTKEY 0xFE0000FE

static vector<string> _chromiumBrowser = {
	"chrome.exe", "brave.exe", "msedge.exe"
};

// Apps that need clipboard paste instead of step-by-step SendInput
// to avoid "nhảy chữ đôi" (double characters) and flickering
static vector<string> _forceClipboardApps = {
	"photoshop.exe", "Photoshop.exe",
	"illustrator.exe", "Illustrator.exe",
	"inkscape.exe",
	"gimp-2.10.exe", "gimp.exe"
};

// Apps that need batch SendInput (all events in one call) instead of clipboard.
// Atomic: all backspace + char events in one SendInput call, no clipboard/Ctrl+V.
// Note: Photoshop does NOT work with KEYEVENTF_UNICODE — use clipboard path instead.
static vector<string> _batchSendInputApps = {
	"CorelDRW.exe", "coreldraw.exe"
};

extern int vSendKeyStepByStep;
extern int vUseGrayIcon;
extern int vShowOnStartUp;
extern int vRunWithWindows;
extern int vTerminalSpeed;

static HHOOK hKeyboardHook;
static HHOOK hMouseHook;
static HWINEVENTHOOK hSystemEvent;
static KBDLLHOOKSTRUCT* keyboardData;
static MSLLHOOKSTRUCT* mouseData;
static vKeyHookState* pData;
static vector<Uint16> _syncKey;
static Uint32 _flag = 0, _lastFlag = 0, _privateFlag;
static bool _flagChanged = false, _isFlagKey;
static Uint16 _keycode;
static Uint16 _newChar, _newCharHi;

static vector<Uint16> _newCharString;
static Uint16 _newCharSize;
static bool _willSendControlKey = false;

static Uint16 _uniChar[2];
static int _i, _j, _k;
static Uint32 _tempChar;

static string macroText, macroContent;
static int _languageTemp = 0; //use for smart switch key
static vector<Byte> savedSmartSwitchKeyData; ////use for smart switch key

static bool _hasJustUsedHotKey = false;

static INPUT backspaceEvent[2];
static INPUT keyEvent[2];

// --- PATCH: Anti race-condition & terminal support ---
static volatile bool _isSendingOutput = false;  // busy flag: prevent processing while sending
static TerminalType _currentTermType = TERM_NORMAL_APP;  // cached terminal type
static HWND _lastDetectedHwnd = NULL;  // cache key for terminal detection
static int _backspaceDelayMs = 1;  // delay between backspaces (ms), prevents race condition
static int _postBackspaceDelayMs = 5;  // delay after all backspaces, before sending new chars
static bool _clipboardDirty = false;   // true after we wrote to clipboard for paste

// Persistent worker thread for terminal output.
// Hook NEVER waits — it copies data into queue, signals the event,
// and returns immediately. Thread wakes up and does the slow work.
struct TerminalWorkItem {
	int backspaceCount;
	vector<Uint16> charString;
	int charCount;
	TerminalType termType;
	Uint16 trailingKeycode;  // for macro: key after macro text
	bool isSlowApp;          // true = use select+CtrlV (Photoshop, CorelDRAW)
	int selectCount;         // number of Shift+Left presses (may differ from backspaceCount for double-code)
	bool isBatchSendInput;   // true = batch all events in one SendInput (no clipboard)
};

static CRITICAL_SECTION _termCS;       // protects _termQueue
static HANDLE _termEvent = NULL;       // signaled when new work arrives
static HANDLE _termThread = NULL;      // persistent thread handle
static vector<TerminalWorkItem> _termQueue;  // work queue
static volatile bool _termRunning = false;   // thread alive flag

// --- PATCH: ANSI window detection for legacy apps ---
static HWND _currentFocusedHwnd = NULL;  // actual focused child window
static bool _isAnsiWindow = false;       // cached IsWindowUnicode() result
static HWND _lastAnsiCheckHwnd = NULL;   // cache key for ANSI detection

// Get the actual focused child window (not just foreground window)
static HWND getFocusedWindow() {
	HWND hwnd = GetForegroundWindow();
	if (!hwnd) return NULL;
	DWORD threadId = GetWindowThreadProcessId(hwnd, NULL);
	GUITHREADINFO gti = {};
	gti.cbSize = sizeof(GUITHREADINFO);
	if (GetGUIThreadInfo(threadId, &gti) && gti.hwndFocus)
		return gti.hwndFocus;
	return hwnd;
}

// Update ANSI window cache — call after updateTerminalTypeCache()
static void updateAnsiWindowCache() {
	_currentFocusedHwnd = getFocusedWindow();
	if (_currentFocusedHwnd != _lastAnsiCheckHwnd) {
		_lastAnsiCheckHwnd = _currentFocusedHwnd;
		_isAnsiWindow = _currentFocusedHwnd ? !IsWindowUnicode(_currentFocusedHwnd) : false;
	}
}

// Should we use ANSI path (WM_CHAR) instead of SendInput(KEYEVENTF_UNICODE)?
// True when: ANSI window + single-byte code table (TCVN3/VNI/CP1258) + not terminal
static bool shouldUseAnsiPath() {
	return _isAnsiWindow
		&& (vCodeTable == 1 || vCodeTable == 2 || vCodeTable == 4)
		&& (_currentTermType == TERM_NORMAL_APP || _currentTermType == TERM_WIN32_CONSOLE);
}

// Should we force clipboard paste instead of step-by-step SendInput?
// ANSI windows + Unicode encoding: KEYEVENTF_UNICODE gets converted to ANSI
// by Windows → non-ASCII Vietnamese chars (ư, ờ, etc.) become '?'.
// Clipboard paste (CF_UNICODETEXT) works correctly for these apps.
static bool shouldForceClipboardForAnsi() {
	return _isAnsiWindow
		&& vCodeTable == 0  // Unicode encoding
		&& (_currentTermType == TERM_NORMAL_APP || _currentTermType == TERM_WIN32_CONSOLE);
}

// Force clipboard paste for apps known to have flickering/double-char issues
// with step-by-step SendInput (Photoshop, CorelDRAW, etc.)
static bool shouldForceClipboardForSlowApp() {
	if (_currentTermType != TERM_NORMAL_APP && _currentTermType != TERM_WIN32_CONSOLE)
		return false;
	const string& exe = OpenKeyHelper::getLastAppExecuteName();
	for (const auto& app : _forceClipboardApps) {
		if (_stricmp(exe.c_str(), app.c_str()) == 0)
			return true;
	}
	return false;
}

// Batch SendInput for apps that spin cursor with clipboard operations (CorelDRAW).
// All backspace + unicode char events go in one SendInput call — no clipboard needed.
static bool shouldUseBatchSendInput() {
	if (_currentTermType != TERM_NORMAL_APP && _currentTermType != TERM_WIN32_CONSOLE)
		return false;
	const string& exe = OpenKeyHelper::getLastAppExecuteName();
	for (const auto& app : _batchSendInputApps) {
		if (_stricmp(exe.c_str(), app.c_str()) == 0)
			return true;
	}
	return false;
}
// --- END PATCH ---

// Pending key buffer: real keys consumed while thread is busy.
// Thread re-injects them after finishing current work.
struct PendingKey {
	WPARAM wParam;  // WM_KEYDOWN etc
	DWORD vkCode;
	DWORD scanCode;
	DWORD flags;
};
static vector<PendingKey> _pendingKeys;  // protected by _termCS
// --- END PATCH ---

LRESULT CALLBACK keyboardHookProcess(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK mouseHookProcess(int nCode, WPARAM wParam, LPARAM lParam);
VOID CALLBACK winEventProcCallback(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);
DWORD WINAPI TerminalOutputThread(LPVOID param);  // forward declaration

// --- PATCH: Terminal detection helper ---
static void updateTerminalTypeCache() {
	HWND hwnd = GetForegroundWindow();
	if (hwnd != _lastDetectedHwnd) {
		_lastDetectedHwnd = hwnd;
		_currentTermType = detectTerminalType(hwnd);
	}
}

// Terminal output via worker thread
static void QueueTerminalOutput(int backspaceCount, bool isMacro);
// --- END PATCH ---

void OpenKeyFree() {
	// --- PATCH: Shutdown terminal worker thread ---
	if (_termThread) {
		_termRunning = false;
		SetEvent(_termEvent);
		WaitForSingleObject(_termThread, 1000);
		CloseHandle(_termThread);
		_termThread = NULL;
	}
	if (_termEvent) { CloseHandle(_termEvent); _termEvent = NULL; }
	DeleteCriticalSection(&_termCS);
	// --- END PATCH ---

	UnhookWindowsHookEx(hMouseHook);
	UnhookWindowsHookEx(hKeyboardHook);
	UnhookWinEvent(hSystemEvent);
}

void OpenKeyInit() {
	APP_GET_DATA(vLanguage, 1);
	APP_GET_DATA(vInputType, 0);
	vFreeMark = 0;
	APP_GET_DATA(vCodeTable, 0);
	APP_GET_DATA(vCheckSpelling, 1);
	APP_GET_DATA(vUseModernOrthography, 0);
	APP_GET_DATA(vQuickTelex, 0);
	APP_GET_DATA(vSwitchKeyStatus, 0x7A000206);
	APP_GET_DATA(vRestoreIfWrongSpelling, 1);
	APP_GET_DATA(vFixRecommendBrowser, 1);
	APP_GET_DATA(vUseMacro, 1);
	APP_GET_DATA(vUseMacroInEnglishMode, 0);
	APP_GET_DATA(vAutoCapsMacro, 0);
	APP_GET_DATA(vSendKeyStepByStep, 1);
	APP_GET_DATA(vUseGrayIcon, 0);
	APP_GET_DATA(vShowOnStartUp, 1);
	APP_GET_DATA(vRunWithWindows, 1);
	OpenKeyHelper::registerRunOnStartup(vRunWithWindows);
	APP_GET_DATA(vUseSmartSwitchKey, 1);
	APP_GET_DATA(vUpperCaseFirstChar, 0);
	APP_GET_DATA(vAllowConsonantZFWJ, 0);
	APP_GET_DATA(vTempOffSpelling, 0);
	APP_GET_DATA(vQuickStartConsonant, 0);
	APP_GET_DATA(vQuickEndConsonant, 0);
	APP_GET_DATA(vSupportMetroApp, 0);
	APP_GET_DATA(vRunAsAdmin, 0);
	APP_GET_DATA(vCreateDesktopShortcut, 0);
	APP_GET_DATA(vCheckNewVersion, 0);
	APP_GET_DATA(vRememberCode, 1);
	APP_GET_DATA(vOtherLanguage, 1);
	APP_GET_DATA(vTempOffOpenKey, 0);
	APP_GET_DATA(vFixChromiumBrowser, 0);
	APP_GET_DATA(vTerminalSpeed, 2);  // 0=fastest ... 4=slowest, default=2 (normal)

	//init convert tool
	APP_GET_DATA(convertToolDontAlertWhenCompleted, 0);
	APP_GET_DATA(convertToolToAllCaps, 0);
	APP_GET_DATA(convertToolToAllNonCaps, 0);
	APP_GET_DATA(convertToolToCapsFirstLetter, 0);
	APP_GET_DATA(convertToolToCapsEachWord, 0);
	APP_GET_DATA(convertToolRemoveMark, 0);
	APP_GET_DATA(convertToolFromCode, 0);
	APP_GET_DATA(convertToolToCode, 0);
	APP_GET_DATA(convertToolHotKey, EMPTY_HOTKEY);
	if (convertToolHotKey == 0) {
		convertToolHotKey = EMPTY_HOTKEY;
	}

	pData = (vKeyHookState*)vKeyInit();

	//pre-create back key
	backspaceEvent[0].type = INPUT_KEYBOARD;
	backspaceEvent[0].ki.dwFlags = 0;
	backspaceEvent[0].ki.wVk = VK_BACK;
	backspaceEvent[0].ki.wScan = 0;
	backspaceEvent[0].ki.time = 0;
	backspaceEvent[0].ki.dwExtraInfo = 1;

	backspaceEvent[1].type = INPUT_KEYBOARD;
	backspaceEvent[1].ki.dwFlags = KEYEVENTF_KEYUP;
	backspaceEvent[1].ki.wVk = VK_BACK;
	backspaceEvent[1].ki.wScan = 0;
	backspaceEvent[1].ki.time = 0;
	backspaceEvent[1].ki.dwExtraInfo = 1;

	//get key state
	_flag = 0;
	if (GetKeyState(VK_LSHIFT) < 0 || GetKeyState(VK_RSHIFT) < 0) _flag |= MASK_SHIFT;
	if (GetKeyState(VK_LCONTROL) < 0 || GetKeyState(VK_RCONTROL) < 0) _flag |= MASK_CONTROL;
	if (GetKeyState(VK_LMENU) < 0 || GetKeyState(VK_RMENU) < 0) _flag |= MASK_ALT;
	if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) _flag |= MASK_WIN;
	if (GetKeyState(VK_NUMLOCK) < 0) _flag |= MASK_NUMLOCK;
	if (GetKeyState(VK_CAPITAL) == 1) _flag |= MASK_CAPITAL;
	if (GetKeyState(VK_SCROLL) < 0) _flag |= MASK_SCROLL;

	//init and load macro data
	DWORD macroDataSize;
	BYTE* macroData = OpenKeyHelper::getRegBinary(_T("macroData"), macroDataSize);
	initMacroMap((Byte*)macroData, (int)macroDataSize);

	//init and load smart switch key data
	DWORD smartSwitchKeySize;
	BYTE* data = OpenKeyHelper::getRegBinary(_T("smartSwitchKey"), smartSwitchKeySize);
	initSmartSwitchKey((Byte*)data, (int)smartSwitchKeySize);

	//init hook
	HINSTANCE hInstance = GetModuleHandle(NULL);
	hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboardHookProcess, hInstance, 0);
	hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseHookProcess, hInstance, 0);
	hSystemEvent = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, winEventProcCallback, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

	// --- PATCH: Start persistent terminal worker thread ---
	InitializeCriticalSection(&_termCS);
	_termEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  // auto-reset
	_termRunning = true;
	_termThread = CreateThread(NULL, 0, TerminalOutputThread, NULL, 0, NULL);
	// --- END PATCH ---
}

void saveSmartSwitchKeyData() {
	getSmartSwitchKeySaveData(savedSmartSwitchKeyData);
	OpenKeyHelper::setRegBinary(_T("smartSwitchKey"), savedSmartSwitchKeyData.data(), (int)savedSmartSwitchKeyData.size());
}

static void InsertKeyLength(const Uint8& len) {
	_syncKey.push_back(len);
}

static inline void prepareKeyEvent(INPUT& input, const Uint16& keycode, const bool& isPress, const DWORD& flag=0) {
	input.type = INPUT_KEYBOARD;
	input.ki.dwFlags = isPress ? flag : flag|KEYEVENTF_KEYUP;
	input.ki.wVk = keycode;
	input.ki.wScan = 0;
	input.ki.time = 0;
	input.ki.dwExtraInfo = 1;
}

static inline void prepareUnicodeEvent(INPUT& input, const Uint16& unicode, const bool& isPress) {
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = 0;
	input.ki.wScan = unicode;
	input.ki.time = 0;
	input.ki.dwFlags = (isPress ? 0 : KEYEVENTF_KEYUP) | KEYEVENTF_UNICODE;
	input.ki.dwExtraInfo = 1;
}

static void SendCombineKey(const Uint16& key1, const Uint16& key2, const DWORD& flagKey1=0, const DWORD& flagKey2 = 0) {
	prepareKeyEvent(keyEvent[0], key1, true, flagKey1);
	SendInput(1, keyEvent, sizeof(INPUT));

	prepareKeyEvent(keyEvent[0], key2, true, flagKey2);
	prepareKeyEvent(keyEvent[1], key2, false, flagKey2);
	SendInput(2, keyEvent, sizeof(INPUT));

	prepareKeyEvent(keyEvent[0], key1, false, flagKey1);
	SendInput(1, keyEvent, sizeof(INPUT));
}

// --- PATCH: Send single ANSI byte via WM_CHAR for legacy apps ---
static void SendAnsiChar(BYTE charByte) {
	if (_currentFocusedHwnd)
		PostMessage(_currentFocusedHwnd, WM_CHAR, (WPARAM)charByte, 0);
}
// --- END PATCH ---

static void SendKeyCode(Uint32 data) {
	_newChar = (Uint16)data;
	if (!(data & CHAR_CODE_MASK)) {
		if (IS_DOUBLE_CODE(vCodeTable)) //VNI
			InsertKeyLength(1);

		_newChar = keyCodeToCharacter(data);
		if (_newChar == 0) {
			_newChar = (Uint16)data;
			prepareKeyEvent(keyEvent[0], _newChar, true);
			prepareKeyEvent(keyEvent[1], _newChar, false);
			SendInput(2, keyEvent, sizeof(INPUT));
		} else {
			prepareUnicodeEvent(keyEvent[0], _newChar, true);
			prepareUnicodeEvent(keyEvent[1], _newChar, false);
			SendInput(2, keyEvent, sizeof(INPUT));
		}
	} else {
		if (vCodeTable == 0) { //unicode 2 bytes code
			prepareUnicodeEvent(keyEvent[0], _newChar, true);
			prepareUnicodeEvent(keyEvent[1], _newChar, false);
			SendInput(2, keyEvent, sizeof(INPUT));
		} else if (vCodeTable == 1 || vCodeTable == 2 || vCodeTable == 4) { //others such as VNI Windows, TCVN3: 1 byte code
			_newCharHi = HIBYTE(_newChar);
			_newChar = LOBYTE(_newChar);

			// --- PATCH: ANSI path for legacy apps ---
			if (shouldUseAnsiPath()) {
				SendAnsiChar((BYTE)_newChar);
				if (_newCharHi > 32) {
					if (vCodeTable == 2) //VNI
						InsertKeyLength(2);
					SendAnsiChar((BYTE)_newCharHi);
				} else {
					if (vCodeTable == 2) //VNI
						InsertKeyLength(1);
				}
			} else {
			// --- END PATCH ---
				prepareUnicodeEvent(keyEvent[0], _newChar, true);
				prepareUnicodeEvent(keyEvent[1], _newChar, false);
				SendInput(2, keyEvent, sizeof(INPUT));

				if (_newCharHi > 32) {
					if (vCodeTable == 2) //VNI
						InsertKeyLength(2);
					prepareUnicodeEvent(keyEvent[0], _newCharHi, true);
					prepareUnicodeEvent(keyEvent[1], _newCharHi, false);
					SendInput(2, keyEvent, sizeof(INPUT));
				} else {
					if (vCodeTable == 2) //VNI
						InsertKeyLength(1);
				}
			}
		} else if (vCodeTable == 3) { //Unicode Compound
			_newCharHi = (_newChar >> 13);
			_newChar &= 0x1FFF;
			_uniChar[0] = _newChar;
			_uniChar[1] = _newCharHi > 0 ? (_unicodeCompoundMark[_newCharHi - 1]) : 0;
			InsertKeyLength(_newCharHi > 0 ? 2 : 1);
			prepareUnicodeEvent(keyEvent[0], _uniChar[0], true);
			prepareUnicodeEvent(keyEvent[1], _uniChar[0], false);
			SendInput(2, keyEvent, sizeof(INPUT));
			if (_newCharHi > 0) {
				prepareUnicodeEvent(keyEvent[0], _uniChar[1], true);
				prepareUnicodeEvent(keyEvent[1], _uniChar[1], false);
				SendInput(2, keyEvent, sizeof(INPUT));
			}
		}
	}
}

static void SendBackspace() {
	SendInput(2, backspaceEvent, sizeof(INPUT));
	// --- PATCH: Small delay to let target app process the backspace ---
	if (_backspaceDelayMs > 0) Sleep(_backspaceDelayMs);
	// --- END PATCH ---
	if (vSupportMetroApp && OpenKeyHelper::getLastAppExecuteName().compare("ApplicationFrameHost.exe") == 0) {//Metro App
		SendMessage(HWND_BROADCAST, WM_CHAR, VK_BACK, 0L);
		SendMessage(HWND_BROADCAST, WM_CHAR, VK_BACK, 0L);
	}
	if (IS_DOUBLE_CODE(vCodeTable)) { //VNI or Unicode Compound
		if (_syncKey.back() > 1) {
			/*if (!(vCodeTable == 3 && containUnicodeCompoundApp(FRONT_APP))) {
				SendInput(2, backspaceEvent, sizeof(INPUT));
			}*/
			SendInput(2, backspaceEvent, sizeof(INPUT));
			if (vSupportMetroApp && OpenKeyHelper::getLastAppExecuteName().compare("ApplicationFrameHost.exe") == 0) {//Metro App
				SendMessage(HWND_BROADCAST, WM_CHAR, VK_BACK, 0L);
				SendMessage(HWND_BROADCAST, WM_CHAR, VK_BACK, 0L);
			}
		}
		_syncKey.pop_back();
	}
}

// Select N character positions backward using batched Shift+Left (one SendInput call).
// For slow apps (Photoshop, CorelDRAW): replaces backspace to avoid flickering/lost chars.
// After selection, paste from clipboard replaces selected text atomically.
static void SendSelectBackward(int count) {
	if (count <= 0) return;
	int totalEvents = 2 + count * 2;  // Shift down + Left×N (down+up) + Shift up
	vector<INPUT> inputs(totalEvents);
	memset(inputs.data(), 0, sizeof(INPUT) * totalEvents);

	int idx = 0;
	// Shift down
	inputs[idx].type = INPUT_KEYBOARD;
	inputs[idx].ki.wVk = VK_LSHIFT;
	inputs[idx].ki.dwExtraInfo = 1;
	idx++;

	// Left × count (down + up each)
	for (int i = 0; i < count; i++) {
		inputs[idx].type = INPUT_KEYBOARD;
		inputs[idx].ki.wVk = VK_LEFT;
		inputs[idx].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
		inputs[idx].ki.dwExtraInfo = 1;
		idx++;

		inputs[idx].type = INPUT_KEYBOARD;
		inputs[idx].ki.wVk = VK_LEFT;
		inputs[idx].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
		inputs[idx].ki.dwExtraInfo = 1;
		idx++;
	}

	// Shift up
	inputs[idx].type = INPUT_KEYBOARD;
	inputs[idx].ki.wVk = VK_LSHIFT;
	inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP;
	inputs[idx].ki.dwExtraInfo = 1;

	SendInput(totalEvents, inputs.data(), sizeof(INPUT));
}

// Batch SendInput: all backspace + new char events in ONE SendInput call.
// For apps like CorelDRAW where clipboard/Ctrl+V causes cursor spinning.
// Runs inline in hook — SendInput just queues events, returns instantly.
static void SendBatchOutput(bool isMacro = false) {
	// Step 1: Calculate actual backspace events needed
	int actualBackspaces = pData->backspaceCount;
	if (IS_DOUBLE_CODE(vCodeTable)) {
		actualBackspaces = 0;
		for (int i = 0; i < pData->backspaceCount && !_syncKey.empty(); i++) {
			actualBackspaces += _syncKey.back();
			_syncKey.pop_back();
		}
	}

	// Step 2: Build character list (same logic as SendNewCharString, maintains _syncKey)
	_j = 0;
	_newCharSize = isMacro ? (Uint16)pData->macroData.size() : pData->newCharCount;
	if (_newCharString.size() < (size_t)(_newCharSize + 4)) {
		_newCharString.resize(_newCharSize + 4);
	}
	_willSendControlKey = false;

	if (_newCharSize > 0) {
		for (_k = isMacro ? 0 : pData->newCharCount - 1;
			isMacro ? _k < pData->macroData.size() : _k >= 0;
			isMacro ? _k++ : _k--) {

			_tempChar = DYNA_DATA(isMacro, _k);
			if (_tempChar & PURE_CHARACTER_MASK) {
				_newCharString[_j++] = _tempChar;
				if (IS_DOUBLE_CODE(vCodeTable))
					InsertKeyLength(1);
			} else if (!(_tempChar & CHAR_CODE_MASK)) {
				if (IS_DOUBLE_CODE(vCodeTable))
					InsertKeyLength(1);
				_newCharString[_j++] = keyCodeToCharacter(_tempChar);
			} else {
				_newChar = _tempChar;
				if (vCodeTable == 0) {
					_newCharString[_j++] = _newChar;
				} else if (vCodeTable == 1 || vCodeTable == 2 || vCodeTable == 4) {
					_newCharHi = HIBYTE(_newChar);
					_newChar = LOBYTE(_newChar);
					_newCharString[_j++] = _newChar;
					if (_newCharHi > 32) {
						if (vCodeTable == 2)
							InsertKeyLength(2);
						_newCharString[_j++] = _newCharHi;
						_newCharSize++;
					} else {
						if (vCodeTable == 2)
							InsertKeyLength(1);
					}
				} else if (vCodeTable == 3) {
					_newCharHi = (_newChar >> 13);
					_newChar &= 0x1FFF;
					InsertKeyLength(_newCharHi > 0 ? 2 : 1);
					_newCharString[_j++] = _newChar;
					if (_newCharHi > 0) {
						_newCharSize++;
						_newCharString[_j++] = _unicodeCompoundMark[_newCharHi - 1];
					}
				}
			}
		}
	}

	// Handle vRestore trailing character
	if (!isMacro && (pData->code == vRestore || pData->code == vRestoreAndStartNewSession)) {
		if (keyCodeToCharacter(_keycode) != 0) {
			_newCharString[_j++] = keyCodeToCharacter(
				_keycode | ((_flag & MASK_SHIFT) || (_flag & MASK_CAPITAL) ? CAPS_MASK : 0));
		} else {
			_willSendControlKey = true;
		}
	}

	if (!isMacro && pData->code == vRestoreAndStartNewSession) {
		startNewSession();
	}

	// Step 3: Build INPUT array — backspaces + KEYEVENTF_UNICODE chars
	int charCount = _j;
	int totalEvents = actualBackspaces * 2 + charCount * 2;
	if (totalEvents == 0) return;

	vector<INPUT> batchInputs(totalEvents);
	memset(batchInputs.data(), 0, sizeof(INPUT) * totalEvents);

	int idx = 0;
	// Backspace events (down + up)
	for (int i = 0; i < actualBackspaces; i++) {
		batchInputs[idx].type = INPUT_KEYBOARD;
		batchInputs[idx].ki.wVk = VK_BACK;
		batchInputs[idx].ki.dwExtraInfo = 1;
		idx++;
		batchInputs[idx].type = INPUT_KEYBOARD;
		batchInputs[idx].ki.wVk = VK_BACK;
		batchInputs[idx].ki.dwFlags = KEYEVENTF_KEYUP;
		batchInputs[idx].ki.dwExtraInfo = 1;
		idx++;
	}
	// Character events (KEYEVENTF_UNICODE, down + up)
	for (int i = 0; i < charCount; i++) {
		batchInputs[idx].type = INPUT_KEYBOARD;
		batchInputs[idx].ki.wVk = 0;
		batchInputs[idx].ki.wScan = _newCharString[i];
		batchInputs[idx].ki.dwFlags = KEYEVENTF_UNICODE;
		batchInputs[idx].ki.dwExtraInfo = 1;
		idx++;
		batchInputs[idx].type = INPUT_KEYBOARD;
		batchInputs[idx].ki.wVk = 0;
		batchInputs[idx].ki.wScan = _newCharString[i];
		batchInputs[idx].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
		batchInputs[idx].ki.dwExtraInfo = 1;
		idx++;
	}

	SendInput(totalEvents, batchInputs.data(), sizeof(INPUT));

	// Trailing key sent separately (control keys need VK, not KEYEVENTF_UNICODE)
	if (_willSendControlKey) {
		SendKeyCode(_keycode);
	}
	// Macro trailing key (the trigger key that completed the macro match)
	if (isMacro) {
		SendKeyCode(_keycode | (_flag & MASK_SHIFT ? CAPS_MASK : 0));
	}
}

static void SendEmptyCharacter() {
	if (IS_DOUBLE_CODE(vCodeTable)) //VNI or Unicode Compound
		InsertKeyLength(1);

	// --- PATCH: ANSI path — use regular space instead of Unicode narrow no-break space ---
	if (shouldUseAnsiPath()) {
		SendAnsiChar(0x20);
		return;
	}
	// --- END PATCH ---

	_newChar = 0x202F; //empty char

	prepareUnicodeEvent(keyEvent[0], _newChar, true);
	prepareUnicodeEvent(keyEvent[1], _newChar, false);
	SendInput(2, keyEvent, sizeof(INPUT));
}

static void SendNewCharString(const bool& dataFromMacro = false, bool useCtrlV = false) {
	_j = 0;
	_newCharSize = dataFromMacro ? (Uint16)pData->macroData.size() : pData->newCharCount;
	if (_newCharString.size() < _newCharSize) {
		_newCharString.resize(_newCharSize);
	}
	_willSendControlKey = false;
	
	if (_newCharSize > 0) {
		for (_k = dataFromMacro ? 0 : pData->newCharCount - 1;
			dataFromMacro ? _k < pData->macroData.size() : _k >= 0;
			dataFromMacro ? _k++ : _k--) {

			_tempChar = DYNA_DATA(dataFromMacro, _k);
			if (_tempChar & PURE_CHARACTER_MASK) {
				_newCharString[_j++] = _tempChar;
				if (IS_DOUBLE_CODE(vCodeTable)) {
					InsertKeyLength(1);
				}
			} else if (!(_tempChar & CHAR_CODE_MASK)) {
				if (IS_DOUBLE_CODE(vCodeTable)) //VNI
					InsertKeyLength(1);
				_newCharString[_j++] = keyCodeToCharacter(_tempChar);
			} else {
				_newChar = _tempChar;
				if (vCodeTable == 0) {  //unicode 2 bytes code
					_newCharString[_j++] = _newChar;
				} else if (vCodeTable == 1 || vCodeTable == 2 || vCodeTable == 4) { //others such as VNI Windows, TCVN3: 1 byte code
					_newCharHi = HIBYTE(_newChar);
					_newChar = LOBYTE(_newChar);
					_newCharString[_j++] = _newChar;

					if (_newCharHi > 32) {
						if (vCodeTable == 2) //VNI
							InsertKeyLength(2);
						_newCharString[_j++] = _newCharHi;
						_newCharSize++;
					}
					else {
						if (vCodeTable == 2) //VNI
							InsertKeyLength(1);
					}
				} else if (vCodeTable == 3) { //Unicode Compound
					_newCharHi = (_newChar >> 13);
					_newChar &= 0x1FFF;

					InsertKeyLength(_newCharHi > 0 ? 2 : 1);
					_newCharString[_j++] = _newChar;
					if (_newCharHi > 0) {
						_newCharSize++;
						_newCharString[_j++] = _unicodeCompoundMark[_newCharHi - 1];
					}

				}
			}
		}//end for
	}

	if (pData->code == vRestore || pData->code == vRestoreAndStartNewSession) { //if is restore
		if (keyCodeToCharacter(_keycode) != 0) {
			_newCharSize++;
			_newCharString[_j++] = keyCodeToCharacter(_keycode | ((_flag & MASK_SHIFT) || (_flag & MASK_CAPITAL) ? CAPS_MASK : 0));
		} else {
			_willSendControlKey = true;
		}
	}
	if (pData->code == vRestoreAndStartNewSession) {
		startNewSession();
	}

	// --- PATCH: ANSI clipboard path for legacy apps ---
	if (shouldUseAnsiPath()) {
		// Build ANSI byte buffer from _newCharString (each entry is a single-byte value)
		vector<BYTE> ansiBuffer(_j);
		for (int idx = 0; idx < _j; idx++) {
			ansiBuffer[idx] = (BYTE)(_newCharString[idx] & 0xFF);
		}
		OpenKeyHelper::setClipboardTextAnsi(ansiBuffer.data(), _j);
	} else {
		OpenKeyHelper::setClipboardText((LPCTSTR)_newCharString.data(), _newCharSize + 1, CF_UNICODETEXT);
	}
	// --- END PATCH ---

	// Paste from clipboard
	if (useCtrlV) {
		SendCombineKey(VK_LCONTROL, 'V');  // Ctrl+V for apps that don't support Shift+Insert
	} else {
		SendCombineKey(KEY_LEFT_SHIFT, VK_INSERT, 0, KEYEVENTF_EXTENDEDKEY);
	}
	_clipboardDirty = true;

	//the case when hCode is vRestore or vRestoreAndStartNewSession,
	//the word is invalid and last key is control key such as TAB, LEFT ARROW, RIGHT ARROW,...
	if (_willSendControlKey) {
		SendKeyCode(_keycode);
	}
}

// --- PATCH: Terminal output via WORKER THREAD ---
// Problem: LL keyboard hook timeout (~200ms). Terminal backspaces need
// Sleep() between each one (mintty PTY chain is slow). With 2+ backspaces,
// total time > 200ms → Windows kills the hook → keys leak through.
//
// Solution: Hook captures output data, posts to worker thread, returns
// immediately (<1ms). Worker thread does slow backspace + paste.

// Build character string from engine data into a vector
static void BuildTerminalCharString(vector<Uint16>& outChars, int& outCount) {
	int j = 0;
	Uint16 newCharSize = pData->newCharCount;
	if (outChars.size() < (size_t)(newCharSize + 4)) {
		outChars.resize(newCharSize + 4);
	}
	Uint16 nc, nchi;
	Uint32 tc;
	for (int k = pData->newCharCount - 1; k >= 0; k--) {
		tc = pData->charData[k];
		if (tc & PURE_CHARACTER_MASK) {
			outChars[j++] = (Uint16)tc;
		} else if (!(tc & CHAR_CODE_MASK)) {
			outChars[j++] = keyCodeToCharacter(tc);
		} else {
			nc = (Uint16)tc;
			if (vCodeTable == 0) {
				outChars[j++] = nc;
			} else if (vCodeTable == 1 || vCodeTable == 2 || vCodeTable == 4) {
				nchi = HIBYTE(nc); nc = LOBYTE(nc);
				outChars[j++] = nc;
				if (nchi > 32) { outChars[j++] = nchi; newCharSize++; }
			} else if (vCodeTable == 3) {
				nchi = (nc >> 13); nc &= 0x1FFF;
				outChars[j++] = nc;
				if (nchi > 0) { newCharSize++; outChars[j++] = _unicodeCompoundMark[nchi - 1]; }
			}
		}
	}
	outCount = newCharSize;
}

static void BuildTerminalMacroString(vector<Uint16>& outChars, int& outCount) {
	int j = 0;
	Uint16 newCharSize = (Uint16)pData->macroData.size();
	if (outChars.size() < (size_t)(newCharSize + 4)) {
		outChars.resize(newCharSize + 4);
	}
	Uint16 nc, nchi;
	Uint32 tc;
	for (int k = 0; k < (int)pData->macroData.size(); k++) {
		tc = pData->macroData[k];
		if (tc & PURE_CHARACTER_MASK) {
			outChars[j++] = (Uint16)tc;
		} else if (!(tc & CHAR_CODE_MASK)) {
			outChars[j++] = keyCodeToCharacter(tc);
		} else {
			nc = (Uint16)tc;
			if (vCodeTable == 0) {
				outChars[j++] = nc;
			} else if (vCodeTable == 1 || vCodeTable == 2 || vCodeTable == 4) {
				nchi = HIBYTE(nc); nc = LOBYTE(nc);
				outChars[j++] = nc;
				if (nchi > 32) { outChars[j++] = nchi; newCharSize++; }
			} else if (vCodeTable == 3) {
				nchi = (nc >> 13); nc &= 0x1FFF;
				outChars[j++] = nc;
				if (nchi > 0) { newCharSize++; outChars[j++] = _unicodeCompoundMark[nchi - 1]; }
			}
		}
	}
	outCount = newCharSize;
}

// Map vTerminalSpeed (0-4) to delay values in milliseconds
static int getTerminalBackspaceDelay() {
	switch (vTerminalSpeed) {
		case 0: return 40;   // Nhanh nhất
		case 1: return 80;   // Nhanh
		case 2: return 120;  // Bình thường (default)
		case 3: return 180;  // Chậm
		case 4: return 250;  // Chậm nhất
		default: return 120;
	}
}
static int getTerminalSettleDelay() {
	switch (vTerminalSpeed) {
		case 0: return 20;
		case 1: return 40;
		case 2: return 80;
		case 3: return 120;
		case 4: return 150;
		default: return 80;
	}
}
static int getTerminalPasteDelay() {
	switch (vTerminalSpeed) {
		case 0: return 30;
		case 1: return 50;
		case 2: return 80;
		case 3: return 100;
		case 4: return 150;
		default: return 80;
	}
}

// Persistent worker thread — sleeps until signaled, processes queue
DWORD WINAPI TerminalOutputThread(LPVOID param) {
	UINT bsScanCode = MapVirtualKey(VK_BACK, MAPVK_VK_TO_VSC);

	while (_termRunning) {
		WaitForSingleObject(_termEvent, INFINITE);
		if (!_termRunning) break;

		// Drain ALL queued work items
		while (true) {
			TerminalWorkItem work;
			work.backspaceCount = 0;
			work.charCount = 0;
			work.trailingKeycode = 0;
			bool hasWork = false;

			EnterCriticalSection(&_termCS);
			if (!_termQueue.empty()) {
				work = _termQueue.front();
				_termQueue.erase(_termQueue.begin());
				hasWork = true;
			}
			LeaveCriticalSection(&_termCS);

			if (!hasWork) break;

			// Step 0: Force-release all modifier keys
			// After previous Shift+Insert paste, Shift may be "stuck"
			{
				INPUT mods[4] = {};
				mods[0].type = INPUT_KEYBOARD;
				mods[0].ki.wVk = VK_SHIFT;
				mods[0].ki.dwFlags = KEYEVENTF_KEYUP;
				mods[0].ki.dwExtraInfo = 1;

				mods[1].type = INPUT_KEYBOARD;
				mods[1].ki.wVk = VK_CONTROL;
				mods[1].ki.dwFlags = KEYEVENTF_KEYUP;
				mods[1].ki.dwExtraInfo = 1;

				mods[2].type = INPUT_KEYBOARD;
				mods[2].ki.wVk = VK_INSERT;
				mods[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
				mods[2].ki.dwExtraInfo = 1;

				mods[3].type = INPUT_KEYBOARD;
				mods[3].ki.wVk = VK_MENU;
				mods[3].ki.dwFlags = KEYEVENTF_KEYUP;
				mods[3].ki.dwExtraInfo = 1;

				SendInput(4, mods, sizeof(INPUT));
				Sleep(15);
			}

			if (work.isBatchSendInput) {
				// --- Batch SendInput path (Photoshop, CorelDRAW, etc.) ---
				// All backspace + char events in one SendInput call. No clipboard.
				// Worker thread gives app time to process before accepting next key.
				int totalEvents = work.backspaceCount * 2 + work.charCount * 2;
				if (totalEvents > 0) {
					vector<INPUT> batchInputs(totalEvents);
					memset(batchInputs.data(), 0, sizeof(INPUT) * totalEvents);
					int idx = 0;
					for (int i = 0; i < work.backspaceCount; i++) {
						batchInputs[idx].type = INPUT_KEYBOARD;
						batchInputs[idx].ki.wVk = VK_BACK;
						batchInputs[idx].ki.dwExtraInfo = 1;
						idx++;
						batchInputs[idx].type = INPUT_KEYBOARD;
						batchInputs[idx].ki.wVk = VK_BACK;
						batchInputs[idx].ki.dwFlags = KEYEVENTF_KEYUP;
						batchInputs[idx].ki.dwExtraInfo = 1;
						idx++;
					}
					for (int i = 0; i < work.charCount; i++) {
						batchInputs[idx].type = INPUT_KEYBOARD;
						batchInputs[idx].ki.wVk = 0;
						batchInputs[idx].ki.wScan = work.charString[i];
						batchInputs[idx].ki.dwFlags = KEYEVENTF_UNICODE;
						batchInputs[idx].ki.dwExtraInfo = 1;
						idx++;
						batchInputs[idx].type = INPUT_KEYBOARD;
						batchInputs[idx].ki.wVk = 0;
						batchInputs[idx].ki.wScan = work.charString[i];
						batchInputs[idx].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
						batchInputs[idx].ki.dwExtraInfo = 1;
						idx++;
					}
					SendInput(totalEvents, batchInputs.data(), sizeof(INPUT));
					Sleep(30);  // let app process batch events
				}
				// Trailing key (macro trigger or vRestore control key)
				if (work.trailingKeycode != 0) {
					Sleep(10);
					SendKeyCode(work.trailingKeycode);
				}
			} else if (work.isSlowApp) {
				// --- Slow app path (Illustrator, Inkscape, etc.) ---
				// Select backward instead of backspace, then Ctrl+V paste.
				// Generous delays: hook already returned, no timeout concern.

				// Step 1: Select backward
				if (work.selectCount > 0) {
					SendSelectBackward(work.selectCount);
					Sleep(30);  // let app process selection
				}

				// Step 2: Clipboard + Ctrl+V
				if (work.charCount > 0) {
					OpenKeyHelper::setClipboardText(
						(LPCTSTR)work.charString.data(),
						work.charCount + 1, CF_UNICODETEXT);
					SendCombineKey(VK_LCONTROL, 'V');
					_clipboardDirty = true;
					Sleep(30);  // let app process paste
				}

				// Step 3: Trailing key (macro or control key)
				if (work.trailingKeycode != 0) {
					Sleep(10);
					SendKeyCode(work.trailingKeycode);
				}
			} else {
				// --- Terminal path ---
				// Step 1: Send backspaces one at a time
				// CRITICAL: dwExtraInfo MUST be non-zero (1) so the LL hook
				// passes them through via the early-return check.
				for (int i = 0; i < work.backspaceCount; i++) {
					INPUT bs[2] = {};
					bs[0].type = INPUT_KEYBOARD;
					bs[0].ki.wVk = VK_BACK;
					bs[0].ki.wScan = bsScanCode;
					bs[0].ki.dwFlags = 0;
					bs[0].ki.dwExtraInfo = 1;

					bs[1].type = INPUT_KEYBOARD;
					bs[1].ki.wVk = VK_BACK;
					bs[1].ki.wScan = bsScanCode;
					bs[1].ki.dwFlags = KEYEVENTF_KEYUP;
					bs[1].ki.dwExtraInfo = 1;

					SendInput(2, bs, sizeof(INPUT));
					Sleep(getTerminalBackspaceDelay());
				}
				if (work.backspaceCount > 0) Sleep(getTerminalSettleDelay());

				// Step 2: Clipboard paste
				if (work.charCount > 0) {
					OpenKeyHelper::setClipboardText(
						(LPCTSTR)work.charString.data(),
						work.charCount + 1, CF_UNICODETEXT);

					if (work.termType == TERM_MINTTY) {
						SendCombineKey(KEY_LEFT_SHIFT, VK_INSERT, 0, KEYEVENTF_EXTENDEDKEY);
					} else if (work.termType == TERM_WINDOWS_TERMINAL) {
						INPUT inputs[6] = {};
						prepareKeyEvent(inputs[0], VK_CONTROL, true);
						prepareKeyEvent(inputs[1], VK_SHIFT, true);
						prepareKeyEvent(inputs[2], 'V', true);
						prepareKeyEvent(inputs[3], 'V', false);
						prepareKeyEvent(inputs[4], VK_SHIFT, false);
						prepareKeyEvent(inputs[5], VK_CONTROL, false);
						SendInput(6, inputs, sizeof(INPUT));
					} else {
						SendCombineKey(KEY_LEFT_SHIFT, VK_INSERT, 0, KEYEVENTF_EXTENDEDKEY);
					}
					Sleep(getTerminalPasteDelay());
				}

				// Step 3: Trailing key (macro)
				if (work.trailingKeycode != 0) {
					Sleep(10);
					SendKeyCode(work.trailingKeycode);
				}
			} // end terminal path
		}

		// Done with all queued work — clear busy flag
		_isSendingOutput = false;

		// Re-inject any keys that were consumed while we were busy.
		// They'll go through the hook normally (engine processes them).
		vector<PendingKey> pending;
		EnterCriticalSection(&_termCS);
		pending.swap(_pendingKeys);
		LeaveCriticalSection(&_termCS);

		for (size_t i = 0; i < pending.size(); i++) {
			Sleep(5);  // minimal gap — just enough for hook to process previous key

			// If a previous re-injected key triggered new terminal output,
			// _isSendingOutput is now true again. Push remaining pending keys
			// back so they get captured in the next cycle.
			if (_isSendingOutput) {
				EnterCriticalSection(&_termCS);
				for (size_t j = i; j < pending.size(); j++) {
					_pendingKeys.push_back(pending[j]);
				}
				LeaveCriticalSection(&_termCS);
				break;
			}

			// Batch keyDown + keyUp in one SendInput call (atomic, no gap)
			INPUT keyPair[2] = {};
			keyPair[0].type = INPUT_KEYBOARD;
			keyPair[0].ki.wVk = (WORD)pending[i].vkCode;
			keyPair[0].ki.wScan = (WORD)pending[i].scanCode;
			keyPair[0].ki.dwFlags = 0;
			keyPair[0].ki.dwExtraInfo = 0;

			keyPair[1].type = INPUT_KEYBOARD;
			keyPair[1].ki.wVk = (WORD)pending[i].vkCode;
			keyPair[1].ki.wScan = (WORD)pending[i].scanCode;
			keyPair[1].ki.dwFlags = KEYEVENTF_KEYUP;
			keyPair[1].ki.dwExtraInfo = 0;

			SendInput(2, keyPair, sizeof(INPUT));
		}
	}
	return 0;
}

// Called from hook: copies data, enqueues, signals thread — NEVER WAITS
static void QueueTerminalOutput(int backspaceCount, bool isMacro) {
	TerminalWorkItem work;
	work.backspaceCount = backspaceCount;
	work.termType = _currentTermType;
	work.trailingKeycode = 0;
	work.charCount = 0;
	work.isSlowApp = false;
	work.selectCount = 0;
	work.isBatchSendInput = false;

	if (isMacro) {
		BuildTerminalMacroString(work.charString, work.charCount);
		work.trailingKeycode = _keycode | (_flag & MASK_SHIFT ? CAPS_MASK : 0);
	} else {
		BuildTerminalCharString(work.charString, work.charCount);
		if (pData->code == vRestore || pData->code == vRestoreAndStartNewSession) {
			Uint16 ch = keyCodeToCharacter(
				_keycode | ((_flag & MASK_SHIFT) || (_flag & MASK_CAPITAL) ? CAPS_MASK : 0));
			if (ch != 0) {
				if (work.charString.size() <= (size_t)work.charCount)
					work.charString.resize(work.charCount + 2);
				work.charString[work.charCount] = ch;
				work.charCount++;
			}
		}
		if (pData->code == vRestoreAndStartNewSession) {
			startNewSession();
		}
	}

	EnterCriticalSection(&_termCS);
	_termQueue.push_back(work);
	LeaveCriticalSection(&_termCS);

	_isSendingOutput = true;
	SetEvent(_termEvent);
}

// Queue slow app output to worker thread (Photoshop, CorelDRAW, etc.)
// Uses select+CtrlV instead of backspace+ShiftInsert
static void QueueSlowAppOutput(int backspaceCount, bool isMacro) {
	TerminalWorkItem work;
	work.backspaceCount = backspaceCount;
	work.termType = TERM_NORMAL_APP;
	work.trailingKeycode = 0;
	work.charCount = 0;
	work.isSlowApp = true;
	work.isBatchSendInput = false;

	// Compute selectCount from _syncKey for double-code tables
	work.selectCount = backspaceCount;
	if (IS_DOUBLE_CODE(vCodeTable)) {
		work.selectCount = 0;
		for (int i = 0; i < backspaceCount && !_syncKey.empty(); i++) {
			work.selectCount += _syncKey.back();
			_syncKey.pop_back();
		}
	}

	if (isMacro) {
		BuildTerminalMacroString(work.charString, work.charCount);
		work.trailingKeycode = _keycode | (_flag & MASK_SHIFT ? CAPS_MASK : 0);
	} else {
		BuildTerminalCharString(work.charString, work.charCount);
		if (pData->code == vRestore || pData->code == vRestoreAndStartNewSession) {
			Uint16 ch = keyCodeToCharacter(
				_keycode | ((_flag & MASK_SHIFT) || (_flag & MASK_CAPITAL) ? CAPS_MASK : 0));
			if (ch != 0) {
				if (work.charString.size() <= (size_t)work.charCount)
					work.charString.resize(work.charCount + 2);
				work.charString[work.charCount] = ch;
				work.charCount++;
			} else {
				// Control key (TAB, arrow, etc.) — send after paste
				work.trailingKeycode = _keycode;
			}
		}
		if (pData->code == vRestoreAndStartNewSession) {
			startNewSession();
		}
	}

	EnterCriticalSection(&_termCS);
	_termQueue.push_back(work);
	LeaveCriticalSection(&_termCS);

	_isSendingOutput = true;
	SetEvent(_termEvent);
}

// Queue batch SendInput to worker thread (Photoshop, CorelDRAW, etc.)
// Hook builds char list (maintaining _syncKey), worker thread sends + waits.
// This keeps _isSendingOutput true until app has time to process the batch.
static void QueueBatchOutput(int backspaceCount, bool isMacro) {
	TerminalWorkItem work;
	work.termType = TERM_NORMAL_APP;
	work.trailingKeycode = 0;
	work.charCount = 0;
	work.isSlowApp = false;
	work.selectCount = 0;
	work.isBatchSendInput = true;

	// Compute actual backspace count from _syncKey for double-code tables
	work.backspaceCount = backspaceCount;
	if (IS_DOUBLE_CODE(vCodeTable)) {
		work.backspaceCount = 0;
		for (int i = 0; i < backspaceCount && !_syncKey.empty(); i++) {
			work.backspaceCount += _syncKey.back();
			_syncKey.pop_back();
		}
	}

	// Build character list with _syncKey maintenance (must run in hook thread)
	_j = 0;
	_newCharSize = isMacro ? (Uint16)pData->macroData.size() : pData->newCharCount;
	if (_newCharString.size() < (size_t)(_newCharSize + 4)) {
		_newCharString.resize(_newCharSize + 4);
	}

	if (_newCharSize > 0) {
		for (_k = isMacro ? 0 : pData->newCharCount - 1;
			isMacro ? _k < pData->macroData.size() : _k >= 0;
			isMacro ? _k++ : _k--) {

			_tempChar = DYNA_DATA(isMacro, _k);
			if (_tempChar & PURE_CHARACTER_MASK) {
				_newCharString[_j++] = _tempChar;
				if (IS_DOUBLE_CODE(vCodeTable))
					InsertKeyLength(1);
			} else if (!(_tempChar & CHAR_CODE_MASK)) {
				if (IS_DOUBLE_CODE(vCodeTable))
					InsertKeyLength(1);
				_newCharString[_j++] = keyCodeToCharacter(_tempChar);
			} else {
				_newChar = _tempChar;
				if (vCodeTable == 0) {
					_newCharString[_j++] = _newChar;
				} else if (vCodeTable == 1 || vCodeTable == 2 || vCodeTable == 4) {
					_newCharHi = HIBYTE(_newChar);
					_newChar = LOBYTE(_newChar);
					_newCharString[_j++] = _newChar;
					if (_newCharHi > 32) {
						if (vCodeTable == 2) InsertKeyLength(2);
						_newCharString[_j++] = _newCharHi;
						_newCharSize++;
					} else {
						if (vCodeTable == 2) InsertKeyLength(1);
					}
				} else if (vCodeTable == 3) {
					_newCharHi = (_newChar >> 13);
					_newChar &= 0x1FFF;
					InsertKeyLength(_newCharHi > 0 ? 2 : 1);
					_newCharString[_j++] = _newChar;
					if (_newCharHi > 0) {
						_newCharSize++;
						_newCharString[_j++] = _unicodeCompoundMark[_newCharHi - 1];
					}
				}
			}
		}
	}

	// Handle trailing character/key
	if (isMacro) {
		work.trailingKeycode = _keycode | (_flag & MASK_SHIFT ? CAPS_MASK : 0);
	} else if (pData->code == vRestore || pData->code == vRestoreAndStartNewSession) {
		Uint16 ch = keyCodeToCharacter(
			_keycode | ((_flag & MASK_SHIFT) || (_flag & MASK_CAPITAL) ? CAPS_MASK : 0));
		if (ch != 0) {
			_newCharString[_j++] = ch;
		} else {
			work.trailingKeycode = _keycode;
		}
	}
	if (!isMacro && pData->code == vRestoreAndStartNewSession) {
		startNewSession();
	}

	// Copy char data to work item
	work.charCount = _j;
	work.charString.resize(_j);
	for (int i = 0; i < _j; i++) {
		work.charString[i] = _newCharString[i];
	}

	EnterCriticalSection(&_termCS);
	_termQueue.push_back(work);
	LeaveCriticalSection(&_termCS);

	_isSendingOutput = true;
	SetEvent(_termEvent);
}
// --- END PATCH ---


bool checkHotKey(int hotKeyData, bool checkKeyCode = true) {
	if ((hotKeyData & (~0x8000)) == EMPTY_HOTKEY)
		return false;
	if (HAS_CONTROL(hotKeyData) ^ GET_BOOL(_lastFlag & MASK_CONTROL))
		return false;
	if (HAS_OPTION(hotKeyData) ^ GET_BOOL(_lastFlag & MASK_ALT))
		return false;
	if (HAS_COMMAND(hotKeyData) ^ GET_BOOL(_lastFlag & MASK_WIN))
		return false;
	if (HAS_SHIFT(hotKeyData) ^ GET_BOOL(_lastFlag & MASK_SHIFT))
		return false;
	if (checkKeyCode) {
		if (GET_SWITCH_KEY(hotKeyData) != _keycode)
			return false;
	}
	return true;
}

void switchLanguage() {
	if (vLanguage == 0)
		vLanguage = 1;
	else
		vLanguage = 0;
	if (HAS_BEEP(vSwitchKeyStatus))
		MessageBeep(MB_OK);
	AppDelegate::getInstance()->onInputMethodChangedFromHotKey();
	if (vUseSmartSwitchKey) {
		setAppInputMethodStatus(OpenKeyHelper::getFrontMostAppExecuteName(), vLanguage | (vCodeTable << 1));
		saveSmartSwitchKeyData();
	}
	startNewSession();
}

static void SendPureCharacter(const Uint16& ch) {
	if (ch < 128)
		SendKeyCode(ch);
	else {
		// --- PATCH: ANSI path for legacy apps ---
		if (shouldUseAnsiPath()) {
			SendAnsiChar((BYTE)(ch & 0xFF));
		} else {
		// --- END PATCH ---
			prepareUnicodeEvent(keyEvent[0], ch, true);
			prepareUnicodeEvent(keyEvent[1], ch, false);
			SendInput(2, keyEvent, sizeof(INPUT));
		}
		if (IS_DOUBLE_CODE(vCodeTable)) {
			InsertKeyLength(1);
		}
	}
}

static void handleMacro() {
	//fix autocomplete
	if (vFixRecommendBrowser) {
		SendEmptyCharacter();
		pData->backspaceCount++;
	}

	//send backspace
	if (pData->backspaceCount > 0) {
		for (int i = 0; i < pData->backspaceCount; i++) {
			SendBackspace();
		}
		// --- PATCH: Wait for backspaces to be processed ---
		if (_postBackspaceDelayMs > 0) Sleep(_postBackspaceDelayMs);
		// --- END PATCH ---
	}
	//send real data
	// --- PATCH: Force clipboard for ANSI window + Unicode encoding ---
	if (!vSendKeyStepByStep || shouldForceClipboardForAnsi()) {
	// --- END PATCH ---
		SendNewCharString(true);
	} else {
		for (int i = 0; i < pData->macroData.size(); i++) {
			if (pData->macroData[i] & PURE_CHARACTER_MASK) {
				SendPureCharacter(pData->macroData[i]);
			} else {
				SendKeyCode(pData->macroData[i]);
			}
		}
	}
	SendKeyCode(_keycode | (_flag & MASK_SHIFT ? CAPS_MASK : 0));
}

static bool SetModifierMask(const Uint16& vkCode) {
	// For caps lock case, toggling the flag isn't enough. We need to check the actual state, which should be done before each key press.
	// Example: the caps lock state can be changed without the key being pressed, or the key toggle is made with admin privilege, making the app not able to detect the change.
	if (GetKeyState(VK_CAPITAL) == 1) _flag |= MASK_CAPITAL;
	else _flag &= ~MASK_CAPITAL;

	if (vkCode == VK_LSHIFT || vkCode == VK_RSHIFT) _flag |= MASK_SHIFT;
	else if (vkCode == VK_LCONTROL || vkCode == VK_RCONTROL) _flag |= MASK_CONTROL;
	else if (vkCode == VK_LMENU || vkCode == VK_RMENU) _flag |= MASK_ALT;
	else if (vkCode == VK_LWIN || vkCode == VK_RWIN) _flag |= MASK_WIN;
	else if (vkCode == VK_NUMLOCK) _flag |= MASK_NUMLOCK;
	else if (vkCode == VK_SCROLL) _flag |= MASK_SCROLL;
	else { 
		_isFlagKey = false;
		return false; 
	}
	_isFlagKey = true;
	return true;
}

static bool UnsetModifierMask(const Uint16& vkCode) {
	if (vkCode == VK_LSHIFT || vkCode == VK_RSHIFT) _flag &= ~MASK_SHIFT;
	else if (vkCode == VK_LCONTROL || vkCode == VK_RCONTROL) _flag &= ~MASK_CONTROL;
	else if (vkCode == VK_LMENU || vkCode == VK_RMENU) _flag &= ~MASK_ALT;
	else if (vkCode == VK_LWIN || vkCode == VK_RWIN) _flag &= ~MASK_WIN;
	else if (vkCode == VK_NUMLOCK) _flag &= ~MASK_NUMLOCK;
	else if (vkCode == VK_SCROLL) _flag &= ~MASK_SCROLL;
	else { 
		_isFlagKey = false;
		return false; 
	}
	_isFlagKey = true;
	return true;
}

LRESULT CALLBACK keyboardHookProcess(int nCode, WPARAM wParam, LPARAM lParam) {
	keyboardData = (KBDLLHOOKSTRUCT *)lParam;
	//ignore my event
	if (keyboardData->dwExtraInfo != 0) {
		return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
	}

	// --- PATCH: Skip processing while worker thread is sending output ---
	if (_isSendingOutput) {
		// CONSUME key and queue for re-injection after worker thread finishes.
		// Works for both terminals and slow apps (Photoshop, CorelDRAW).
		if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
			PendingKey pk;
			pk.wParam = wParam;
			pk.vkCode = keyboardData->vkCode;
			pk.scanCode = keyboardData->scanCode;
			pk.flags = keyboardData->flags;
			EnterCriticalSection(&_termCS);
			_pendingKeys.push_back(pk);
			LeaveCriticalSection(&_termCS);
		}
		return -1;  // consume key
	}
	// --- END PATCH ---

	//ignore if IME pad is open when typing Japanese/Chinese...
	HWND hWnd = GetForegroundWindow();
	// --- PATCH: Safer IME check - skip for terminals (they don't use IME) ---
	updateTerminalTypeCache();
	updateAnsiWindowCache();
	if (_currentTermType == TERM_NORMAL_APP || _currentTermType == TERM_WIN32_CONSOLE) {
		HWND hIME = ImmGetDefaultIMEWnd(hWnd);
		if (hIME) {
			LRESULT isImeON = SendMessage(hIME, WM_IME_CONTROL, IMC_GETOPENSTATUS, 0);
			if (isImeON) {
				return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
			}
		}
	}
	// --- END PATCH ---
	
	//check modifier key
	if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
		//LOG(L"Key down: %d\n", keyboardData->vkCode);
		SetModifierMask((Uint16)keyboardData->vkCode);
	} else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
		//LOG(L"Key up: %d\n", keyboardData->vkCode);
		UnsetModifierMask((Uint16)keyboardData->vkCode);
	}
	if (!_isFlagKey && wParam != WM_KEYUP && wParam != WM_SYSKEYUP)
		_keycode = (Uint16)keyboardData->vkCode;

	//switch language shortcut; convert hotkey
	if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && !_isFlagKey && _keycode != 0) {
		if (GET_SWITCH_KEY(vSwitchKeyStatus) != _keycode && GET_SWITCH_KEY(convertToolHotKey) != _keycode) {
			_lastFlag = 0;
		} else {
			if (GET_SWITCH_KEY(vSwitchKeyStatus) == _keycode && checkHotKey(vSwitchKeyStatus, GET_SWITCH_KEY(vSwitchKeyStatus) != 0xFE)) {
				switchLanguage();
				_hasJustUsedHotKey = true;
				_keycode = 0;
				return -1;
			}
			if (GET_SWITCH_KEY(convertToolHotKey) == _keycode && checkHotKey(convertToolHotKey, GET_SWITCH_KEY(convertToolHotKey) != 0xFE)) {
				AppDelegate::getInstance()->onQuickConvert();
				_hasJustUsedHotKey = true;
				_keycode = 0;
				return -1;
			}
		}
		_hasJustUsedHotKey = _lastFlag != 0;
	} else if (_isFlagKey) {
		if (_lastFlag == 0 || _lastFlag < _flag)
			_lastFlag = _flag;
		else if (_lastFlag > _flag) {
			//check switch
			if (checkHotKey(vSwitchKeyStatus, GET_SWITCH_KEY(vSwitchKeyStatus) != 0xFE)) {
				switchLanguage();
				_hasJustUsedHotKey = true;
			}
			if (checkHotKey(convertToolHotKey, GET_SWITCH_KEY(convertToolHotKey) != 0xFE)) {
				AppDelegate::getInstance()->onQuickConvert();
				_hasJustUsedHotKey = true;
			}
			//check temporarily turn off spell checking
			if (vTempOffSpelling && !_hasJustUsedHotKey && _lastFlag & MASK_CONTROL) {
				vTempOffSpellChecking();
			}
			if (vTempOffOpenKey && !_hasJustUsedHotKey && _lastFlag & MASK_ALT) {
				vTempOffEngine();
			}
			_lastFlag = _flag;
			_hasJustUsedHotKey = false;
		}
		_keycode = 0;
		return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
	}

	//if is in english mode
	if (vLanguage == 0) {
		if (vUseMacro && vUseMacroInEnglishMode && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
			vEnglishMode(((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) ? vKeyEventState::KeyDown : vKeyEventState::MouseDown),
				_keycode,
				(_flag & MASK_SHIFT) || (_flag & MASK_CAPITAL),
				OTHER_CONTROL_KEY);

			if (pData->code == vReplaceMaro) { //handle macro in english mode
				handleMacro();
				return NULL;
			}
		}
		return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
	}

	//handle keyboard
	if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
		//send event signal to Engine
		vKeyHandleEvent(vKeyEvent::Keyboard,
						vKeyEventState::KeyDown,
						_keycode,
						(_flag & MASK_SHIFT && _flag & MASK_CAPITAL) ? 0 : (_flag & MASK_SHIFT ? 1 : (_flag & MASK_CAPITAL ? 2 : 0)),
						OTHER_CONTROL_KEY);
		if (pData->code == vDoNothing) { //do nothing
			// --- PATCH: Clear clipboard after paste on word-break key (space, etc.) ---
			if (_clipboardDirty && pData->extCode == 1) {
				if (OpenClipboard(0)) {
					EmptyClipboard();
					CloseClipboard();
				}
				_clipboardDirty = false;
			}
			// --- END PATCH ---
			if (IS_DOUBLE_CODE(vCodeTable)) { //VNI
				if (pData->extCode == 1) { //break key
					_syncKey.clear();
				} else if (pData->extCode == 2) { //delete key
					if (_syncKey.size() > 0) {
						if (_syncKey.back() > 1 && (vCodeTable == 2 || vCodeTable == 3)) {
							//send one more backspace
							SendInput(2, backspaceEvent, sizeof(INPUT));
						}
						_syncKey.pop_back();
					}
				} else if (pData->extCode == 3) { //normal key
					InsertKeyLength(1);
				}
			}
			return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
		} else if (pData->code == vWillProcess || pData->code == vRestore || pData->code == vRestoreAndStartNewSession) { //handle result signal
			// --- PATCH: Set busy flag to prevent processing new keys while sending ---
			_isSendingOutput = true;
			// --- END PATCH ---

			// --- PATCH: Terminal path: worker thread does backspace + paste ---
			if (_currentTermType != TERM_NORMAL_APP && _currentTermType != TERM_WIN32_CONSOLE) {
				// Terminal: queue output to worker thread, return immediately
				QueueTerminalOutput(pData->backspaceCount, false);
				// _isSendingOutput stays true — thread clears it when done
			} else if (shouldForceClipboardForSlowApp()) {
				// --- PATCH: Slow app path — queue to worker thread (same as terminal) ---
				// Hook returns immediately; worker thread does select + Ctrl+V paste
				QueueSlowAppOutput(pData->backspaceCount, false);
				// _isSendingOutput stays true — thread clears it when done
			} else if (shouldUseBatchSendInput()) {
				// --- PATCH: Batch SendInput via worker thread (Photoshop, CorelDRAW) ---
				// Worker thread sends batch + sleeps for app to process.
				QueueBatchOutput(pData->backspaceCount, false);
				// _isSendingOutput stays true — thread clears it when done
			} else {
			// --- END PATCH ---

				//fix autocomplete
				if (vFixRecommendBrowser && pData->extCode != 4) {
					if (vFixChromiumBrowser &&
						std::find(_chromiumBrowser.begin(), _chromiumBrowser.end(), OpenKeyHelper::getLastAppExecuteName()) != _chromiumBrowser.end()) {
						SendCombineKey(KEY_LEFT_SHIFT, KEY_LEFT, 0, KEYEVENTF_EXTENDEDKEY);
						if (pData->backspaceCount == 1)
							pData->backspaceCount--;
					} else {
						SendEmptyCharacter();
						pData->backspaceCount++;
					}
				}

				//send backspace
				if (pData->backspaceCount > 0 && pData->backspaceCount < MAX_BUFF) {
					for (_i = 0; _i < pData->backspaceCount; _i++) {
						SendBackspace();
					}
					// --- PATCH: Wait for app to process all backspaces before sending new chars ---
					if (_postBackspaceDelayMs > 0) Sleep(_postBackspaceDelayMs);
					// --- END PATCH ---
				}

				//send new character
				// --- PATCH: Force clipboard for ANSI window + Unicode encoding ---
				if (!vSendKeyStepByStep || shouldForceClipboardForAnsi()) {
				// --- END PATCH ---
					SendNewCharString();
				} else {
					if (pData->newCharCount > 0 && pData->newCharCount <= MAX_BUFF) {
						for (int i = pData->newCharCount - 1; i >= 0; i--) {
							SendKeyCode(pData->charData[i]);
						}
					}
					if (pData->code == vRestore || pData->code == vRestoreAndStartNewSession) {
						SendKeyCode(_keycode | ((_flag & MASK_CAPITAL) || (_flag & MASK_SHIFT) ? CAPS_MASK : 0));
					}
					if (pData->code == vRestoreAndStartNewSession) {
						startNewSession();
					}
				}

				// --- PATCH: Clear busy flag ---
				_isSendingOutput = false;
			} // --- end normal app path ---
			// --- END PATCH ---
		} else if (pData->code == vReplaceMaro) { //MACRO
			// --- PATCH: Terminal & slow app aware macro ---
			_isSendingOutput = true;
			if (_currentTermType != TERM_NORMAL_APP && _currentTermType != TERM_WIN32_CONSOLE) {
				// Terminal: queue to worker thread (includes trailing key)
				QueueTerminalOutput(pData->backspaceCount, true);
			} else if (shouldForceClipboardForSlowApp()) {
				// Slow app: queue to worker thread (select + Ctrl+V)
				QueueSlowAppOutput(pData->backspaceCount, true);
			} else if (shouldUseBatchSendInput()) {
				// Batch SendInput via worker thread (Photoshop, CorelDRAW)
				QueueBatchOutput(pData->backspaceCount, true);
				// _isSendingOutput stays true — thread clears it when done
			} else {
				handleMacro();
				_isSendingOutput = false;
			}
			// --- END PATCH ---
		}
		return -1; //consume event
	}
	return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

LRESULT CALLBACK mouseHookProcess(int nCode, WPARAM wParam, LPARAM lParam) {
	mouseData = (MSLLHOOKSTRUCT *)lParam;
	switch (wParam) {
	case WM_LBUTTONDOWN:
	
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN:
	case WM_NCXBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
	case WM_XBUTTONUP:
	case WM_NCXBUTTONUP:
		//send event signal to Engine
		vKeyHandleEvent(vKeyEvent::Mouse, vKeyEventState::MouseDown, 0);
		if (IS_DOUBLE_CODE(vCodeTable)) { //VNI
			_syncKey.clear();
		}
		// --- PATCH: Invalidate ANSI cache on mouse click (focus may change) ---
		_lastAnsiCheckHwnd = NULL;
		// --- END PATCH ---
		break;
	}
	return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
}

VOID CALLBACK winEventProcCallback(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
	// --- PATCH: Invalidate terminal + ANSI cache on window switch ---
	_lastDetectedHwnd = NULL;
	_lastAnsiCheckHwnd = NULL;
	// --- END PATCH ---

	//smart switch key
	if (vUseSmartSwitchKey || vRememberCode) {
		string& exe = OpenKeyHelper::getFrontMostAppExecuteName();
		if (exe.compare("explorer.exe") == 0) //dont apply with windows explorer
			return;
		_languageTemp = getAppInputMethodStatus(exe, vLanguage | (vCodeTable << 1));
		vTempOffEngine(false);
		if (vUseSmartSwitchKey && (_languageTemp & 0x01) != vLanguage) {
			if (_languageTemp != -1) {
				vLanguage = _languageTemp;
				AppDelegate::getInstance()->onInputMethodChangedFromHotKey();
			} else {
				saveSmartSwitchKeyData();
			}
		}
		startNewSession();
		if (vRememberCode && (_languageTemp >> 1) != vCodeTable) { //for remember table code feature
			if (_languageTemp != -1) {
				AppDelegate::getInstance()->onTableCode(_languageTemp >> 1);
			} else {
				saveSmartSwitchKeyData();
			}
		}
		if (vSupportMetroApp && exe.compare("ApplicationFrameHost.exe") == 0) {//Metro App
			SendMessage(HWND_BROADCAST, WM_CHAR, VK_BACK, 0L);
			SendMessage(HWND_BROADCAST, WM_CHAR, VK_BACK, 0L);
		}
	}
}
