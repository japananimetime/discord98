#include "VoicePanel.hpp"
#include "ScreenPickerDialog.hpp"
#include "core/DiscordInstance.hpp"
#include "core/voice/VoiceManager.hpp"
#include "core/stream/StreamManager.hpp"

#define VOICE_PANEL_COLOR COLOR_3DFACE

WNDCLASS VoicePanel::g_VoicePanelClass;

VoicePanel::VoicePanel()
{
	m_buttons[BTN_MUTE]       = Button(BTN_MUTE);
	m_buttons[BTN_DEAFEN]     = Button(BTN_DEAFEN);
	m_buttons[BTN_GOLIVE]     = Button(BTN_GOLIVE);
	m_buttons[BTN_DISCONNECT] = Button(BTN_DISCONNECT);
}

VoicePanel::~VoicePanel()
{
	if (m_hwnd)
	{
		BOOL b = DestroyWindow(m_hwnd);
		assert(b && "was window deleted?");
		m_hwnd = NULL;
	}
}

void VoicePanel::Update()
{
	InvalidateRect(m_hwnd, NULL, FALSE);
}

void VoicePanel::UpdateVisibility()
{
	DiscordInstance* pInst = GetDiscordInstance();
	if (!pInst) {
		ShowWindow(m_hwnd, SW_HIDE);
		return;
	}

	VoiceManager& vm = pInst->GetVoiceManager();
	bool voiceActive = vm.IsConnected() || vm.IsConnecting() || vm.IsWaitingForServer();

	if (voiceActive)
		ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
	else
		ShowWindow(m_hwnd, SW_HIDE);
}

void VoicePanel::Layout()
{
	RECT rcClient = {};
	GetClientRect(m_hwnd, &rcClient);

	int btnSize = ScaleByDPI(24);
	int btnPad  = ScaleByDPI(4);
	int rightMargin = ScaleByDPI(4);

	// Vertically center buttons to align with both text lines
	int panelH = rcClient.bottom - rcClient.top;
	int btnY = rcClient.top + (panelH - btnSize) / 2;
	int btnX = rcClient.right - rightMargin;

	// Disconnect is rightmost
	btnX -= btnSize;
	SetRect(&m_buttons[BTN_DISCONNECT].m_rect, btnX, btnY, btnX + btnSize, btnY + btnSize);

	btnX -= btnPad + btnSize;
	SetRect(&m_buttons[BTN_GOLIVE].m_rect, btnX, btnY, btnX + btnSize, btnY + btnSize);

	btnX -= btnPad + btnSize;
	SetRect(&m_buttons[BTN_DEAFEN].m_rect, btnX, btnY, btnX + btnSize, btnY + btnSize);

	btnX -= btnPad + btnSize;
	SetRect(&m_buttons[BTN_MUTE].m_rect, btnX, btnY, btnX + btnSize, btnY + btnSize);
}

int VoicePanel::GetButtonIcon(eVoiceButton btn)
{
	DiscordInstance* pInst = GetDiscordInstance();
	if (!pInst)
		return IDI_CANCEL;

	VoiceManager& vm = pInst->GetVoiceManager();

	switch (btn)
	{
		case BTN_MUTE:
			return vm.IsMuted() ? IDI_MIC_OFF : IDI_MIC;
		case BTN_DEAFEN:
			return vm.IsDeafened() ? IDI_STATUS_DND : IDI_VOICE;
		case BTN_GOLIVE:
		{
			StreamManager& sm = pInst->GetStreamManager();
			// Use CHANNEL icon as stand-in for screen share (monitor-like icon)
			return (sm.IsStreaming() || sm.IsConnecting()) ? IDI_CHANNEL_UNREAD : IDI_CHANNEL;
		}
		case BTN_DISCONNECT:
			return IDI_CANCEL;
		default:
			return IDI_CANCEL;
	}
}

static void DrawScreenShareIcon(HDC hdc, int x, int y, int size, bool streaming)
{
	// Draw a monitor-shaped icon using GDI
	// Monitor body
	int monW = size;
	int monH = size * 3 / 4;
	int standW = size / 3;
	int standH = size / 6;
	int monY = y;
	int standY = monY + monH;

	COLORREF iconColor = IsTextColorLight() ? RGB(255, 255, 255) : RGB(0, 0, 0);
	HPEN pen = CreatePen(PS_SOLID, 1, iconColor);
	HPEN oldPen = (HPEN)SelectObject(hdc, pen);
	HBRUSH oldBrush;

	if (streaming)
	{
		// Fill screen area green when streaming
		HBRUSH greenBrush = CreateSolidBrush(RGB(0, 180, 0));
		oldBrush = (HBRUSH)SelectObject(hdc, greenBrush);
		Rectangle(hdc, x, monY, x + monW, monY + monH);
		SelectObject(hdc, oldBrush);
		DeleteObject(greenBrush);
	}
	else
	{
		// Just outline when idle
		oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
		Rectangle(hdc, x, monY, x + monW, monY + monH);
		SelectObject(hdc, oldBrush);

		// Inner screen area (inset by 1-2px)
		int inset = size > 12 ? 2 : 1;
		RECT rcScreen = { x + inset, monY + inset, x + monW - inset, monY + monH - inset };
		HBRUSH screenBrush = CreateSolidBrush(IsTextColorLight() ? RGB(200, 200, 200) : RGB(80, 80, 80));
		FillRect(hdc, &rcScreen, screenBrush);
		DeleteObject(screenBrush);
	}

	// Stand: vertical stem + base
	int stemX = x + monW / 2;
	MoveToEx(hdc, stemX, standY, NULL);
	LineTo(hdc, stemX, standY + standH);

	// Base line
	int baseLeft = x + (monW - standW) / 2;
	MoveToEx(hdc, baseLeft, standY + standH, NULL);
	LineTo(hdc, baseLeft + standW, standY + standH);

	SelectObject(hdc, oldPen);
	DeleteObject(pen);
}

void VoicePanel::DrawButton(HDC hdc, Button& button)
{
	int iconSize = ScaleByDPI(16);
	int sizeX = button.m_rect.right  - button.m_rect.left;
	int sizeY = button.m_rect.bottom - button.m_rect.top;
	int iconX = button.m_rect.left + sizeX / 2 - iconSize / 2;
	int iconY = button.m_rect.top  + sizeY / 2 - iconSize / 2;

	// Draw 3D button edge
	RECT rcBtn = button.m_rect;
	UINT edgeFlags = BDR_RAISEDOUTER | BDR_RAISEDINNER;
	if (button.m_held)
		edgeFlags = BDR_SUNKENOUTER | BDR_SUNKENINNER;
	else if (button.m_hot)
		edgeFlags = BDR_RAISEDOUTER;

	FillRect(hdc, &rcBtn, GetSysColorBrush(VOICE_PANEL_COLOR));
	DrawEdge(hdc, &rcBtn, edgeFlags, BF_RECT);

	// Offset icon if held
	if (button.m_held) {
		iconX += ScaleByDPI(1);
		iconY += ScaleByDPI(1);
	}

	// BTN_GOLIVE: draw a custom monitor icon instead of loading an icon resource
	if (button.m_id == BTN_GOLIVE)
	{
		DiscordInstance* pInst = GetDiscordInstance();
		bool streaming = false;
		if (pInst)
		{
			StreamManager& sm = pInst->GetStreamManager();
			streaming = sm.IsStreaming() || sm.IsConnecting();
		}
		DrawScreenShareIcon(hdc, iconX, iconY, iconSize, streaming);
		return;
	}

	int iconID = GetButtonIcon(button.m_id);
	HICON hicon = (HICON) ri::LoadImage(g_hInstance, MAKEINTRESOURCE(DMIC(iconID)), IMAGE_ICON, iconSize, iconSize, LR_SHARED | LR_CREATEDIBSECTION);
	if (!hicon)
		hicon = LoadIcon(g_hInstance, MAKEINTRESOURCE(iconID));

	if (hicon) {
		DrawIconInvert(hdc, hicon, iconX, iconY, iconSize, iconSize,
			IsIconMostlyBlack(hicon) && IsTextColorLight());
	}
}

void VoicePanel::OnButtonClicked(eVoiceButton btn)
{
	DiscordInstance* pInst = GetDiscordInstance();
	if (!pInst)
		return;

	VoiceManager& vm = pInst->GetVoiceManager();

	switch (btn)
	{
		case BTN_MUTE:
			vm.ToggleMute();
			break;
		case BTN_DEAFEN:
			vm.ToggleDeafen();
			break;
		case BTN_GOLIVE:
		{
			StreamManager& sm = pInst->GetStreamManager();
			if (sm.IsStreaming() || sm.IsConnecting())
			{
				sm.StopStream();
			}
			else if (vm.IsConnected())
			{
				// Show screen picker dialog
				ScreenPickerResult pickerResult;
				if (ShowScreenPickerDialog(g_Hwnd, pickerResult))
				{
					StreamManager::StreamSource source;
					source.useWindow = pickerResult.useWindow;
					source.adapterIndex = pickerResult.adapterIndex;
					source.outputIndex = pickerResult.outputIndex;
					source.hwnd = pickerResult.hwnd;
					sm.SetStreamSource(source);
					sm.StartStream(vm.GetGuildID(), vm.GetChannelID());
				}
			}
			break;
		}
		case BTN_DISCONNECT:
			vm.LeaveVoiceChannel();
			break;
	}

	// Voice state change will be broadcast via WM_VOICESTATECHANGE,
	// which will trigger UpdateVisibility and Update
}

void VoicePanel::Paint(HDC hdc)
{
	RECT rcClient = {};
	GetClientRect(m_hwnd, &rcClient);

	// Fill background
	FillRect(hdc, &rcClient, GetSysColorBrush(VOICE_PANEL_COLOR));

	DiscordInstance* pInst = GetDiscordInstance();
	if (!pInst)
		return;

	VoiceManager& vm = pInst->GetVoiceManager();

	// Determine status text and color
	LPCTSTR statusText;
	COLORREF statusColor;
	if (vm.IsConnected()) {
		statusText = TEXT("Voice Connected");
		statusColor = RGB(0, 128, 0); // green
	}
	else if (vm.IsConnecting()) {
		statusText = TEXT("Connecting...");
		statusColor = RGB(200, 150, 0); // yellow
	}
	else {
		statusText = TEXT("Waiting...");
		statusColor = RGB(200, 150, 0); // yellow
	}

	int smIcon = GetSystemMetrics(SM_CXSMICON);
	int margin = ScaleByDPI(6);
	int textX = margin + smIcon + ScaleByDPI(4);

	// Draw voice icon
	int iconID = DMIC(IDI_VOICE);
	HICON hVoiceIcon = (HICON) ri::LoadImage(g_hInstance, MAKEINTRESOURCE(iconID), IMAGE_ICON, smIcon, smIcon, LR_SHARED | LR_CREATEDIBSECTION);
	if (!hVoiceIcon)
		hVoiceIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_VOICE));

	int iconY = margin;
	if (hVoiceIcon) {
		ri::DrawIconEx(hdc, margin, iconY, hVoiceIcon, smIcon, smIcon, 0, NULL, DI_NORMAL | DI_COMPAT);
	}

	// Draw status text
	COLORREF oldText = SetTextColor(hdc, statusColor);
	COLORREF oldBk   = SetBkColor(hdc, GetSysColor(VOICE_PANEL_COLOR));
	HGDIOBJ  oldFont = SelectObject(hdc, g_AccountInfoFont);

	RECT rcStatus;
	SetRect(&rcStatus, textX, margin, rcClient.right - margin, margin + smIcon);
	DrawText(hdc, statusText, -1, &rcStatus, DT_SINGLELINE | DT_VCENTER | ri::GetWordEllipsisFlag());

	// Draw channel name below
	std::string channelName = "#" + vm.GetChannelName();
	LPTSTR tstrChannel = ConvertCppStringToTString(channelName);

	SelectObject(hdc, g_AccountTagFont);
	SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));

	RECT rcChannel;
	SetRect(&rcChannel, textX, margin + smIcon + ScaleByDPI(2), rcClient.right - margin, rcClient.bottom);
	DrawText(hdc, tstrChannel, -1, &rcChannel, DT_SINGLELINE | ri::GetWordEllipsisFlag());

	free(tstrChannel);

	SelectObject(hdc, oldFont);
	SetTextColor(hdc, oldText);
	SetBkColor(hdc, oldBk);

	// Draw buttons
	Layout();
	for (int i = 0; i < BTN_COUNT; i++)
		DrawButton(hdc, m_buttons[i]);
}

LRESULT CALLBACK VoicePanel::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	VoicePanel* pThis = (VoicePanel*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

	switch (uMsg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
			SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
			break;
		}
		case WM_DESTROY:
		{
			SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR) NULL);
			pThis->m_hwnd = NULL;
			break;
		}
		case WM_PAINT:
		{
			PAINTSTRUCT ps = {};
			HDC hdc = BeginPaint(hWnd, &ps);
			pThis->Paint(hdc);
			EndPaint(hWnd, &ps);
			break;
		}
		case WM_PRINT:
		case WM_PRINTCLIENT:
			pThis->Paint((HDC) wParam);
			break;

		case WM_MOUSEMOVE:
		case WM_MOUSELEAVE:
		{
			assert(pThis);
			HDC hdc = GetDC(hWnd);

			if (uMsg == WM_MOUSEMOVE)
			{
				TRACKMOUSEEVENT tme;
				tme.cbSize = sizeof tme;
				tme.dwFlags = TME_LEAVE;
				tme.dwHoverTime = 1;
				tme.hwndTrack = hWnd;
				ri::TrackMouseEvent(&tme);

				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				for (int i = 0; i < BTN_COUNT; i++)
				{
					Button& btn = pThis->m_buttons[i];
					if (pThis->m_bLClickHeld)
					{
						bool hit = PtInRect(&btn.m_rect, pt);
						if (btn.m_held != hit || btn.m_hot) {
							btn.m_held = hit;
							btn.m_hot = false;
							pThis->DrawButton(hdc, btn);
						}
					}
					else
					{
						bool hit = PtInRect(&btn.m_rect, pt);
						if (btn.m_hot != hit) {
							btn.m_hot = hit;
							pThis->DrawButton(hdc, btn);
						}
					}
				}
			}
			else // WM_MOUSELEAVE
			{
				POINT pt = { -1, -1 };
				for (int i = 0; i < BTN_COUNT; i++)
				{
					Button& btn = pThis->m_buttons[i];
					if (btn.m_hot || btn.m_held) {
						btn.m_hot = false;
						btn.m_held = false;
						pThis->DrawButton(hdc, btn);
					}
				}
				pThis->m_bLClickHeld = false;
			}

			ReleaseDC(hWnd, hdc);
			break;
		}
		case WM_LBUTTONDOWN:
		{
			assert(pThis);
			pThis->m_bLClickHeld = true;
			POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			HDC hdc = GetDC(hWnd);
			for (int i = 0; i < BTN_COUNT; i++)
			{
				Button& btn = pThis->m_buttons[i];
				bool hit = PtInRect(&btn.m_rect, pt);
				if (btn.m_held != hit || btn.m_hot) {
					btn.m_held = hit;
					btn.m_hot = false;
					pThis->DrawButton(hdc, btn);
				}
			}
			ReleaseDC(hWnd, hdc);
			break;
		}
		case WM_LBUTTONUP:
		{
			assert(pThis);
			pThis->m_bLClickHeld = false;
			POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			HDC hdc = GetDC(hWnd);

			for (int i = 0; i < BTN_COUNT; i++)
			{
				Button& btn = pThis->m_buttons[i];
				if (btn.m_held) {
					btn.m_held = false;
					pThis->DrawButton(hdc, btn);

					if (PtInRect(&btn.m_rect, pt))
						pThis->OnButtonClicked(btn.m_id);
				}
				// Update hot state
				bool hit = PtInRect(&btn.m_rect, pt);
				if (btn.m_hot != hit) {
					btn.m_hot = hit;
					pThis->DrawButton(hdc, btn);
				}
			}

			ReleaseDC(hWnd, hdc);
			break;
		}
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void VoicePanel::InitializeClass()
{
	WNDCLASS& wc = g_VoicePanelClass;

	wc.lpszClassName = T_VOICE_PANEL_CLASS;
	wc.hbrBackground = ri::GetSysColorBrush(VOICE_PANEL_COLOR);
	wc.style         = 0;
	wc.hCursor       = LoadCursor(0, IDC_ARROW);
	wc.lpfnWndProc   = VoicePanel::WndProc;
	wc.hInstance      = g_hInstance;

	RegisterClass(&wc);
}

VoicePanel* VoicePanel::Create(HWND hwnd, LPRECT pRect, int id)
{
	VoicePanel* newThis = new VoicePanel;

	int width  = pRect->right - pRect->left;
	int height = pRect->bottom - pRect->top;

	newThis->m_hwnd = CreateWindowEx(
		0,
		T_VOICE_PANEL_CLASS,
		NULL,
		WS_CHILD,  // not visible by default
		pRect->left, pRect->top,
		width, height,
		hwnd,
		(HMENU)(uintptr_t)id,
		g_hInstance,
		newThis
	);

	return newThis;
}
