#include "ScreenPickerDialog.hpp"
#include <dxgi.h>
#include <dwmapi.h>
#include <tchar.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

#define IDC_SOURCE_LIST  1001
#define IDC_TAB_CONTROL  1002

#define THUMB_W 160
#define THUMB_H 90

static std::vector<ScreenSource> g_sources;
static int g_selectedIndex = -1;
static HWND g_hList = NULL;
static HWND g_hTab = NULL;
static HIMAGELIST g_hImageList = NULL;

static HBITMAP CaptureMonitorThumbnail(int adapterIndex, int outputIndex, int thumbW, int thumbH)
{
	HBITMAP hbm = NULL;

	IDXGIFactory* pFactory = nullptr;
	if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory)))
		return NULL;

	IDXGIAdapter* pAdapter = nullptr;
	if (FAILED(pFactory->EnumAdapters(adapterIndex, &pAdapter))) {
		pFactory->Release();
		return NULL;
	}

	IDXGIOutput* pOutput = nullptr;
	if (FAILED(pAdapter->EnumOutputs(outputIndex, &pOutput))) {
		pAdapter->Release();
		pFactory->Release();
		return NULL;
	}

	DXGI_OUTPUT_DESC desc;
	pOutput->GetDesc(&desc);

	int monW = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
	int monH = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

	// Capture via GDI
	HDC hScreen = CreateDC(desc.DeviceName, NULL, NULL, NULL);
	if (hScreen)
	{
		HDC hMemDC = CreateCompatibleDC(hScreen);
		hbm = CreateCompatibleBitmap(hScreen, thumbW, thumbH);
		HGDIOBJ old = SelectObject(hMemDC, hbm);

		SetStretchBltMode(hMemDC, HALFTONE);
		StretchBlt(hMemDC, 0, 0, thumbW, thumbH,
			hScreen, 0, 0, monW, monH, SRCCOPY);

		SelectObject(hMemDC, old);
		DeleteDC(hMemDC);
		DeleteDC(hScreen);
	}

	pOutput->Release();
	pAdapter->Release();
	pFactory->Release();
	return hbm;
}

static HBITMAP CaptureWindowThumbnail(HWND hwnd, int thumbW, int thumbH)
{
	RECT rc;
	if (!GetWindowRect(hwnd, &rc))
		return NULL;

	int winW = rc.right - rc.left;
	int winH = rc.bottom - rc.top;
	if (winW <= 0 || winH <= 0)
		return NULL;

	HDC hScreen = GetDC(NULL);
	HDC hCaptureDC = CreateCompatibleDC(hScreen);
	HBITMAP hCaptureBmp = CreateCompatibleBitmap(hScreen, winW, winH);
	HGDIOBJ oldCap = SelectObject(hCaptureDC, hCaptureBmp);

	// Try PrintWindow first (works even for partially occluded windows)
	if (!PrintWindow(hwnd, hCaptureDC, PW_RENDERFULLCONTENT))
	{
		// Fallback to BitBlt
		HDC hWinDC = GetWindowDC(hwnd);
		BitBlt(hCaptureDC, 0, 0, winW, winH, hWinDC, 0, 0, SRCCOPY);
		ReleaseDC(hwnd, hWinDC);
	}

	// Scale down to thumbnail
	HDC hThumbDC = CreateCompatibleDC(hScreen);
	HBITMAP hThumb = CreateCompatibleBitmap(hScreen, thumbW, thumbH);
	HGDIOBJ oldThumb = SelectObject(hThumbDC, hThumb);

	SetStretchBltMode(hThumbDC, HALFTONE);
	StretchBlt(hThumbDC, 0, 0, thumbW, thumbH,
		hCaptureDC, 0, 0, winW, winH, SRCCOPY);

	SelectObject(hThumbDC, oldThumb);
	DeleteDC(hThumbDC);

	SelectObject(hCaptureDC, oldCap);
	DeleteObject(hCaptureBmp);
	DeleteDC(hCaptureDC);
	ReleaseDC(NULL, hScreen);

	return hThumb;
}

static void EnumerateMonitors()
{
	IDXGIFactory* pFactory = nullptr;
	if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory)))
		return;

	for (UINT a = 0; ; a++)
	{
		IDXGIAdapter* pAdapter = nullptr;
		if (FAILED(pFactory->EnumAdapters(a, &pAdapter)))
			break;

		DXGI_ADAPTER_DESC adapterDesc;
		pAdapter->GetDesc(&adapterDesc);

		for (UINT o = 0; ; o++)
		{
			IDXGIOutput* pOutput = nullptr;
			if (FAILED(pAdapter->EnumOutputs(o, &pOutput)))
				break;

			DXGI_OUTPUT_DESC outputDesc;
			pOutput->GetDesc(&outputDesc);

			ScreenSource src;
			src.type = ScreenSource::Monitor;
			src.adapterIndex = (int)a;
			src.outputIndex = (int)o;
			src.hwnd = NULL;

			// Build display name
			char nameBuf[256];
			int monW = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
			int monH = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
			snprintf(nameBuf, sizeof(nameBuf), "Screen %u (%dx%d)", (unsigned)(g_sources.size() + 1), monW, monH);
			src.title = nameBuf;

			src.hThumbnail = CaptureMonitorThumbnail((int)a, (int)o, THUMB_W, THUMB_H);

			g_sources.push_back(std::move(src));

			pOutput->Release();
		}

		pAdapter->Release();
	}

	pFactory->Release();
}

struct EnumWindowsCtx
{
	HWND hwndDialog;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
	EnumWindowsCtx* ctx = (EnumWindowsCtx*)lParam;

	// Skip invisible windows
	if (!IsWindowVisible(hwnd))
		return TRUE;

	// Skip zero-size windows
	RECT rc;
	GetWindowRect(hwnd, &rc);
	if (rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0)
		return TRUE;

	// Skip our own dialog
	if (hwnd == ctx->hwndDialog)
		return TRUE;

	// Skip tool windows and cloaked windows
	LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
	if (exStyle & WS_EX_TOOLWINDOW)
		return TRUE;

	// Skip cloaked windows (UWP background apps)
	BOOL cloaked = FALSE;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	if (cloaked)
		return TRUE;

	// Get window title
	char titleBuf[256] = {};
	GetWindowTextA(hwnd, titleBuf, sizeof(titleBuf));
	if (titleBuf[0] == '\0')
		return TRUE;

	// Skip desktop window
	if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow())
		return TRUE;

	ScreenSource src;
	src.type = ScreenSource::Window;
	src.adapterIndex = 0;
	src.outputIndex = 0;
	src.hwnd = hwnd;
	src.title = titleBuf;
	src.hThumbnail = CaptureWindowThumbnail(hwnd, THUMB_W, THUMB_H);

	g_sources.push_back(std::move(src));

	return TRUE;
}

static void EnumerateWindows(HWND hwndDialog)
{
	EnumWindowsCtx ctx;
	ctx.hwndDialog = hwndDialog;
	EnumWindows(EnumWindowsProc, (LPARAM)&ctx);
}

static void PopulateListView(int tabIndex)
{
	if (!g_hList || !g_hImageList)
		return;

	ListView_DeleteAllItems(g_hList);

	// Clear old image list images
	ImageList_RemoveAll(g_hImageList);

	int itemIndex = 0;
	for (size_t i = 0; i < g_sources.size(); i++)
	{
		ScreenSource& src = g_sources[i];

		// Tab 0 = Screens, Tab 1 = Windows
		if (tabIndex == 0 && src.type != ScreenSource::Monitor)
			continue;
		if (tabIndex == 1 && src.type != ScreenSource::Window)
			continue;

		// Add thumbnail to image list
		int imgIdx = -1;
		if (src.hThumbnail)
			imgIdx = ImageList_Add(g_hImageList, src.hThumbnail, NULL);

		LPTSTR tstr = ConvertCppStringToTString(src.title);

		LVITEM lvi = {};
		lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
		lvi.iItem = itemIndex;
		lvi.pszText = tstr;
		lvi.iImage = imgIdx;
		lvi.lParam = (LPARAM)i;  // store index into g_sources

		ListView_InsertItem(g_hList, &lvi);

		free(tstr);
		itemIndex++;
	}
}

static INT_PTR CALLBACK ScreenPickerDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_INITDIALOG:
		{
			g_sources.clear();
			g_selectedIndex = -1;

			// Create tab control
			RECT rcClient;
			GetClientRect(hDlg, &rcClient);

			int margin = ScaleByDPI(8);
			int tabH = ScaleByDPI(28);
			int btnH = ScaleByDPI(30);
			int btnW = ScaleByDPI(80);

			g_hTab = CreateWindowEx(0, WC_TABCONTROL, TEXT(""),
				WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
				margin, margin,
				rcClient.right - 2 * margin,
				tabH,
				hDlg, (HMENU)IDC_TAB_CONTROL, g_hInstance, NULL);

			SendMessage(g_hTab, WM_SETFONT, (WPARAM)g_MessageTextFont, 0);

			TCITEM tie = {};
			tie.mask = TCIF_TEXT;
			tie.pszText = (LPTSTR)TEXT("Screens");
			TabCtrl_InsertItem(g_hTab, 0, &tie);
			tie.pszText = (LPTSTR)TEXT("Windows");
			TabCtrl_InsertItem(g_hTab, 1, &tie);

			// Create image list for thumbnails
			g_hImageList = ImageList_Create(THUMB_W, THUMB_H, ILC_COLOR24, 16, 16);

			// Create list view
			int listTop = margin + tabH + margin;
			int listH = rcClient.bottom - listTop - btnH - 2 * margin;

			g_hList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, TEXT(""),
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
				margin, listTop,
				rcClient.right - 2 * margin,
				listH,
				hDlg, (HMENU)IDC_SOURCE_LIST, g_hInstance, NULL);

			ListView_SetImageList(g_hList, g_hImageList, LVSIL_NORMAL);
			ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

			// Create OK/Cancel buttons
			int btnY = rcClient.bottom - margin - btnH;
			HWND hOk = CreateWindow(TEXT("BUTTON"), TEXT("Share"),
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
				rcClient.right - margin - 2 * btnW - margin, btnY,
				btnW, btnH,
				hDlg, (HMENU)IDOK, g_hInstance, NULL);
			SendMessage(hOk, WM_SETFONT, (WPARAM)g_MessageTextFont, 0);

			HWND hCancel = CreateWindow(TEXT("BUTTON"), TEXT("Cancel"),
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
				rcClient.right - margin - btnW, btnY,
				btnW, btnH,
				hDlg, (HMENU)IDCANCEL, g_hInstance, NULL);
			SendMessage(hCancel, WM_SETFONT, (WPARAM)g_MessageTextFont, 0);

			// Enumerate sources
			EnumerateMonitors();
			EnumerateWindows(hDlg);

			// Show monitors tab by default
			PopulateListView(0);

			// Select first item if available
			if (ListView_GetItemCount(g_hList) > 0)
			{
				ListView_SetItemState(g_hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
				g_selectedIndex = 0;
			}

			// Center dialog on parent
			HWND hwndParent = GetParent(hDlg);
			if (hwndParent)
			{
				RECT rcParent, rcDlg;
				GetWindowRect(hwndParent, &rcParent);
				GetWindowRect(hDlg, &rcDlg);
				int x = (rcParent.left + rcParent.right) / 2 - (rcDlg.right - rcDlg.left) / 2;
				int y = (rcParent.top + rcParent.bottom) / 2 - (rcDlg.bottom - rcDlg.top) / 2;
				SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
			}

			return TRUE;
		}

		case WM_NOTIFY:
		{
			NMHDR* pnm = (NMHDR*)lParam;

			if (pnm->idFrom == IDC_TAB_CONTROL && pnm->code == TCN_SELCHANGE)
			{
				int sel = TabCtrl_GetCurSel(g_hTab);
				PopulateListView(sel);
			}
			else if (pnm->idFrom == IDC_SOURCE_LIST)
			{
				if (pnm->code == LVN_ITEMCHANGED)
				{
					NMLISTVIEW* pnmlv = (NMLISTVIEW*)lParam;
					if (pnmlv->uNewState & LVIS_SELECTED)
					{
						g_selectedIndex = (int)pnmlv->lParam;
					}
				}
				else if (pnm->code == NM_DBLCLK)
				{
					// Double-click selects and confirms
					if (g_selectedIndex >= 0 && g_selectedIndex < (int)g_sources.size())
						EndDialog(hDlg, IDOK);
				}
			}
			break;
		}

		case WM_COMMAND:
		{
			switch (LOWORD(wParam))
			{
				case IDOK:
					if (g_selectedIndex >= 0 && g_selectedIndex < (int)g_sources.size())
						EndDialog(hDlg, IDOK);
					else
						MessageBeep(MB_ICONEXCLAMATION);
					return TRUE;

				case IDCANCEL:
					EndDialog(hDlg, IDCANCEL);
					return TRUE;
			}
			break;
		}

		case WM_DESTROY:
		{
			if (g_hImageList) {
				ImageList_Destroy(g_hImageList);
				g_hImageList = NULL;
			}
			g_hList = NULL;
			g_hTab = NULL;
			break;
		}
	}

	return FALSE;
}

bool ShowScreenPickerDialog(HWND hwndParent, ScreenPickerResult& result)
{
	// Create dialog template in memory (no resource needed)
	// Allocate enough space for a DLGTEMPLATE
	struct {
		DLGTEMPLATE tmpl;
		WORD menu;
		WORD wndClass;
		WCHAR title[32];
	} dlgData = {};

	int dlgW = 500;
	int dlgH = 420;

	dlgData.tmpl.style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
	dlgData.tmpl.dwExtendedStyle = 0;
	dlgData.tmpl.cdit = 0;  // no controls in template — we create them in WM_INITDIALOG
	dlgData.tmpl.x = 0;
	dlgData.tmpl.y = 0;
	dlgData.tmpl.cx = (SHORT)(dlgW * 4 / GetDialogBaseUnits());
	dlgData.tmpl.cy = (SHORT)(dlgH * 8 / HIWORD(GetDialogBaseUnits()));
	dlgData.menu = 0;
	dlgData.wndClass = 0;
	wcscpy_s(dlgData.title, L"Screen Share");

	// Use pixel-sized dialog
	dlgData.tmpl.cx = (SHORT)dlgW;
	dlgData.tmpl.cy = (SHORT)dlgH;

	INT_PTR ret = DialogBoxIndirect(g_hInstance, &dlgData.tmpl, hwndParent, ScreenPickerDlgProc);

	if (ret == IDOK && g_selectedIndex >= 0 && g_selectedIndex < (int)g_sources.size())
	{
		ScreenSource& src = g_sources[g_selectedIndex];
		result.useWindow = (src.type == ScreenSource::Window);
		result.adapterIndex = src.adapterIndex;
		result.outputIndex = src.outputIndex;
		result.hwnd = src.hwnd;

		g_sources.clear();
		return true;
	}

	g_sources.clear();
	return false;
}
