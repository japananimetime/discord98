#include "StreamViewerWindow.hpp"
#include "core/DiscordInstance.hpp"
#include "core/stream/StreamViewer.hpp"

static HWND g_svHwnd = NULL;
static HWND g_svChildHwnd = NULL;
static HWND g_svCloseBtn = NULL;

static int g_svFrameWidth = 0;
static int g_svFrameHeight = 0;
static HBITMAP g_svBitmap = NULL;
static std::mutex g_svMutex;

#define SV_CLOSE_BTN_ID  1
#define SV_BOTTOM_BAR    ScaleByDPI(36)

void StreamViewerOnFrame(const uint8_t* bgraPixels, int width, int height)
{
	if (!g_svChildHwnd)
		return;

	std::lock_guard<std::mutex> lock(g_svMutex);

	// Create or recreate the bitmap if dimensions changed
	if (width != g_svFrameWidth || height != g_svFrameHeight || !g_svBitmap)
	{
		if (g_svBitmap)
			DeleteObject(g_svBitmap);

		g_svFrameWidth = width;
		g_svFrameHeight = height;

		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = width;
		bmi.bmiHeader.biHeight = -height; // top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		void* bits = nullptr;
		g_svBitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);

		if (g_svBitmap && bits)
			memcpy(bits, bgraPixels, width * height * 4);
	}
	else if (g_svBitmap)
	{
		BITMAP bm;
		GetObject(g_svBitmap, sizeof(bm), &bm);
		if (bm.bmBits)
			memcpy(bm.bmBits, bgraPixels, width * height * 4);
	}

	// Request repaint on UI thread
	PostMessage(g_svChildHwnd, WM_STREAMVIEWERFRAME, 0, 0);
}

static LRESULT CALLBACK StreamViewerChildWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_PAINT:
		case WM_STREAMVIEWERFRAME:
		{
			if (uMsg == WM_STREAMVIEWERFRAME)
			{
				InvalidateRect(hWnd, NULL, FALSE);
				return 0;
			}

			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hWnd, &ps);

			std::lock_guard<std::mutex> lock(g_svMutex);

			if (g_svBitmap && g_svFrameWidth > 0 && g_svFrameHeight > 0)
			{
				RECT rc;
				GetClientRect(hWnd, &rc);
				int winW = rc.right - rc.left;
				int winH = rc.bottom - rc.top;

				// Calculate aspect-ratio preserving destination rect
				float scaleX = (float)winW / g_svFrameWidth;
				float scaleY = (float)winH / g_svFrameHeight;
				float scale = (scaleX < scaleY) ? scaleX : scaleY;

				int dstW = (int)(g_svFrameWidth * scale);
				int dstH = (int)(g_svFrameHeight * scale);
				int dstX = (winW - dstW) / 2;
				int dstY = (winH - dstH) / 2;

				// Fill borders with black
				if (dstX > 0 || dstY > 0)
				{
					HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
					if (dstY > 0) {
						RECT top = { 0, 0, winW, dstY };
						FillRect(hdc, &top, black);
						RECT bot = { 0, dstY + dstH, winW, winH };
						FillRect(hdc, &bot, black);
					}
					if (dstX > 0) {
						RECT left = { 0, dstY, dstX, dstY + dstH };
						FillRect(hdc, &left, black);
						RECT right = { dstX + dstW, dstY, winW, dstY + dstH };
						FillRect(hdc, &right, black);
					}
				}

				// Draw the frame
				HDC memDC = CreateCompatibleDC(hdc);
				HGDIOBJ old = SelectObject(memDC, g_svBitmap);

				SetStretchBltMode(hdc, HALFTONE);
				StretchBlt(hdc, dstX, dstY, dstW, dstH,
					memDC, 0, 0, g_svFrameWidth, g_svFrameHeight, SRCCOPY);

				SelectObject(memDC, old);
				DeleteDC(memDC);
			}
			else
			{
				// No frame yet — show "Connecting..."
				RECT rc;
				GetClientRect(hWnd, &rc);
				FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

				COLORREF oldText = SetTextColor(hdc, RGB(200, 200, 200));
				SetBkMode(hdc, TRANSPARENT);
				HGDIOBJ oldFont = SelectObject(hdc, g_MessageTextFont);

				DrawText(hdc, TEXT("Connecting to stream..."), -1, &rc,
					DT_CENTER | DT_VCENTER | DT_SINGLELINE);

				SelectObject(hdc, oldFont);
				SetTextColor(hdc, oldText);
			}

			EndPaint(hWnd, &ps);
			break;
		}
		case WM_DESTROY:
		{
			std::lock_guard<std::mutex> lock(g_svMutex);
			if (g_svBitmap) {
				DeleteObject(g_svBitmap);
				g_svBitmap = NULL;
			}
			g_svFrameWidth = 0;
			g_svFrameHeight = 0;
			g_svChildHwnd = NULL;
			break;
		}
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK StreamViewerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_SIZE:
		{
			if (!g_svChildHwnd)
				break;

			int w = LOWORD(lParam);
			int h = HIWORD(lParam);
			int barH = SV_BOTTOM_BAR;

			MoveWindow(g_svChildHwnd, 0, 0, w, h - barH, TRUE);

			if (g_svCloseBtn)
			{
				int btnW = ScaleByDPI(80);
				int btnH = barH - ScaleByDPI(8);
				int btnY = h - barH + ScaleByDPI(4);
				MoveWindow(g_svCloseBtn, ScaleByDPI(4), btnY, btnW, btnH, TRUE);
			}
			break;
		}
		case WM_COMMAND:
		{
			if (LOWORD(wParam) == SV_CLOSE_BTN_ID)
			{
				// Stop watching and close
				DiscordInstance* pInst = GetDiscordInstance();
				if (pInst)
					pInst->GetStreamViewer().StopWatching();

				KillStreamViewerWindow();
			}
			break;
		}
		case WM_CLOSE:
		{
			DiscordInstance* pInst = GetDiscordInstance();
			if (pInst)
				pInst->GetStreamViewer().StopWatching();

			KillStreamViewerWindow();
			return 0;
		}
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool RegisterStreamViewerClass()
{
	WNDCLASS wc = {};
	wc.lpfnWndProc = StreamViewerWndProc;
	wc.hInstance = g_hInstance;
	wc.lpszClassName = DM_STREAM_VIEWER_CLASS;
	wc.hbrBackground = ri::GetSysColorBrush(COLOR_3DFACE);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_ICON));

	if (!RegisterClass(&wc))
		return false;

	WNDCLASS wc2 = {};
	wc2.lpfnWndProc = StreamViewerChildWndProc;
	wc2.hInstance = g_hInstance;
	wc2.lpszClassName = DM_STREAM_VIEWER_CHILD_CLASS;
	wc2.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc2.hCursor = LoadCursor(NULL, IDC_ARROW);

	return RegisterClass(&wc2) != 0;
}

void CreateStreamViewerWindow(const std::string& streamerName)
{
	KillStreamViewerWindow();

	std::string title = streamerName + " - Stream";
	LPTSTR tstr = ConvertCppStringToTString(title);

	int winW = 960;
	int winH = 540 + SV_BOTTOM_BAR;

	RECT rc = { 0, 0, winW, winH };
	DWORD dwStyle = WS_OVERLAPPEDWINDOW;
	AdjustWindowRect(&rc, dwStyle, FALSE);

	int adjW = rc.right - rc.left;
	int adjH = rc.bottom - rc.top;

	// Center on parent
	RECT parentRect = {};
	GetWindowRect(g_Hwnd, &parentRect);
	int xPos = (parentRect.left + parentRect.right) / 2 - adjW / 2;
	int yPos = (parentRect.top + parentRect.bottom) / 2 - adjH / 2;

	g_svHwnd = CreateWindow(
		DM_STREAM_VIEWER_CLASS,
		tstr,
		dwStyle,
		xPos, yPos, adjW, adjH,
		NULL, NULL, g_hInstance, NULL);

	free(tstr);

	if (!g_svHwnd)
		return;

	// Create video child window
	RECT rcClient;
	GetClientRect(g_svHwnd, &rcClient);
	int barH = SV_BOTTOM_BAR;

	g_svChildHwnd = CreateWindowEx(
		WS_EX_CLIENTEDGE,
		DM_STREAM_VIEWER_CHILD_CLASS,
		NULL,
		WS_CHILD | WS_VISIBLE,
		0, 0,
		rcClient.right, rcClient.bottom - barH,
		g_svHwnd, NULL, g_hInstance, NULL);

	// Create close button
	int btnW = ScaleByDPI(80);
	int btnH = barH - ScaleByDPI(8);
	int btnY = rcClient.bottom - barH + ScaleByDPI(4);

	g_svCloseBtn = CreateWindow(
		TEXT("BUTTON"),
		TEXT("Stop Watching"),
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		ScaleByDPI(4), btnY, btnW, btnH,
		g_svHwnd, (HMENU)SV_CLOSE_BTN_ID, g_hInstance, NULL);

	SendMessage(g_svCloseBtn, WM_SETFONT, (WPARAM)g_MessageTextFont, 0);

	ShowWindow(g_svHwnd, SW_SHOW);
	UpdateWindow(g_svHwnd);
}

void KillStreamViewerWindow()
{
	if (g_svHwnd)
	{
		DestroyWindow(g_svHwnd);
		g_svHwnd = NULL;
		g_svChildHwnd = NULL;
		g_svCloseBtn = NULL;
	}
}
