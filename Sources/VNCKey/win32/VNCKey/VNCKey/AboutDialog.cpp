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
#include "AboutDialog.h"
#include "AppDelegate.h"

AboutDialog::AboutDialog(const HINSTANCE & hInstance, const int & resourceId)
	: BaseDialog(hInstance, resourceId) {
}

AboutDialog::~AboutDialog() {
}

void AboutDialog::fillData() {
}

INT_PTR AboutDialog::eventProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
	case WM_INITDIALOG:
		this->hDlg = hDlg;
		initDialog();
		return TRUE;
	case WM_COMMAND: {
		int wmId = LOWORD(wParam);
		switch (wmId) {
		case IDM_EXIT:
		case IDBUTTON_OK:
		case IDOK:
			AppDelegate::getInstance()->closeDialog(this);
			break;
		}
		break;
	}
	case WM_NOTIFY:
		switch (((LPNMHDR)lParam)->code) {
		case NM_CLICK:
		case NM_RETURN: {
			PNMLINK link = (PNMLINK)lParam;
			if (link->hdr.idFrom == IDC_SYSLINK_AUTHOR_EMAIL)
				ShellExecute(NULL, _T("open"), _T("mailto:vnc@vietnamchess.vn"), NULL, NULL, SW_SHOWNORMAL);
		}
		break;
		}
	}
	return FALSE;
}

void AboutDialog::initDialog() {
	//dialog icon
	SET_DIALOG_ICON(IDI_APP_ICON);

	wchar_t buffer[256];
	wsprintfW(buffer, _T("Phiên bản %s dành cho Windows – Ngày cập nhật: 02-02-2026"), OpenKeyHelper::getVersionString().c_str());
	SendDlgItemMessage(hDlg, IDC_STATIC_APP_VERSION, WM_SETTEXT, 0, LPARAM(buffer));
}

// Update check removed in VNC-Key
void AboutDialog::onUpdateButton() {
}