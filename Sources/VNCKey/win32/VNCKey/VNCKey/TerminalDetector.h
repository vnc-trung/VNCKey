/*----------------------------------------------------------
OpenKey Patch - Terminal Detector
Detect terminal emulators (mintty/Git Bash, Windows Terminal, etc.)
to choose the correct input method.
-----------------------------------------------------------*/
#pragma once
#include <Windows.h>
#include <psapi.h>
#include <tchar.h>
#include <string>
#include <algorithm>

enum TerminalType {
    TERM_NORMAL_APP = 0,   // Notepad, Chrome, Word -> SendInput works
    TERM_WIN32_CONSOLE,    // cmd.exe, PowerShell -> SendInput works
    TERM_MINTTY,           // Git Bash, MSYS2, Cygwin -> needs clipboard
    TERM_WINDOWS_TERMINAL, // Windows Terminal -> needs clipboard
    TERM_OTHER_TERMINAL    // ConEmu, Cmder, etc.
};

static TerminalType detectTerminalType(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd))
        return TERM_NORMAL_APP;

    // Check window class name (fastest method)
    wchar_t className[256] = { 0 };
    GetClassNameW(hwnd, className, 256);

    if (wcscmp(className, L"mintty") == 0)
        return TERM_MINTTY;
    if (wcscmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0)
        return TERM_WINDOWS_TERMINAL;
    if (wcscmp(className, L"ConsoleWindowClass") == 0)
        return TERM_WIN32_CONSOLE;
    if (wcscmp(className, L"VirtualConsoleClass") == 0) // ConEmu
        return TERM_OTHER_TERMINAL;

    // Fallback: check process name
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) return TERM_NORMAL_APP;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProc) return TERM_NORMAL_APP;

    wchar_t procPath[MAX_PATH] = { 0 };
    DWORD pathSize = MAX_PATH;
    QueryFullProcessImageNameW(hProc, 0, procPath, &pathSize);
    CloseHandle(hProc);

    // Extract filename
    std::wstring fullPath(procPath);
    size_t pos = fullPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        fullPath = fullPath.substr(pos + 1);
    // To lowercase
    std::transform(fullPath.begin(), fullPath.end(), fullPath.begin(), ::towlower);

    if (fullPath == L"mintty.exe")
        return TERM_MINTTY;
    if (fullPath == L"windowsterminal.exe" || fullPath == L"wt.exe")
        return TERM_WINDOWS_TERMINAL;
    if (fullPath == L"alacritty.exe" || fullPath == L"wezterm-gui.exe" ||
        fullPath == L"kitty.exe" || fullPath == L"hyper.exe" ||
        fullPath == L"tabby.exe" || fullPath == L"putty.exe" ||
        fullPath == L"conemu.exe" || fullPath == L"conemu64.exe")
        return TERM_OTHER_TERMINAL;

    return TERM_NORMAL_APP;
}

static bool isTerminalWindow(HWND hwnd) {
    TerminalType t = detectTerminalType(hwnd);
    return t != TERM_NORMAL_APP;
}

static bool isMinttyWindow(HWND hwnd) {
    return detectTerminalType(hwnd) == TERM_MINTTY;
}
