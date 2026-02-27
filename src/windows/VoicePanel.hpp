#pragma once

#include "Main.hpp"

#define T_VOICE_PANEL_CLASS TEXT("VoicePanel")

class VoicePanel
{
public:
	HWND m_hwnd = NULL;

	enum eVoiceButton {
		BTN_MUTE = 0,
		BTN_DEAFEN,
		BTN_GOLIVE,
		BTN_DISCONNECT,
		BTN_COUNT
	};

	struct Button {
		RECT m_rect;
		bool m_hot = false;
		bool m_held = false;
		eVoiceButton m_id;

		Button() : m_id(BTN_MUTE) { SetRectEmpty(&m_rect); }
		Button(eVoiceButton id) : m_id(id) { SetRectEmpty(&m_rect); }
	};

public:
	VoicePanel();
	~VoicePanel();

	void Update();
	void UpdateVisibility();
	void Paint(HDC hdc);

private:
	Button m_buttons[BTN_COUNT];
	bool m_bLClickHeld = false;

	void Layout();
	void DrawButton(HDC hdc, Button& button);
	void OnButtonClicked(eVoiceButton btn);
	int GetButtonIcon(eVoiceButton btn);

public:
	static WNDCLASS g_VoicePanelClass;

	static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static void InitializeClass();

	static VoicePanel* Create(HWND hwnd, LPRECT pRect, int id);
};
