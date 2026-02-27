#pragma once

#include <functional>
#include <thread>
#include <atomic>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class ScreenCapture
{
public:
	ScreenCapture();
	~ScreenCapture();

	bool Init(int adapterIndex = 0, int outputIndex = 0);
	bool InitFromWindow(HWND hwnd);
	void Shutdown();

	void Start(int targetFPS = 30);
	void Stop();

	using FrameCallback = std::function<void(
		ID3D11Texture2D* texture, int width, int height, uint32_t timestamp90kHz)>;
	void SetFrameCallback(FrameCallback cb) { m_frameCallback = std::move(cb); }

	ID3D11Device* GetDevice() const { return m_device.Get(); }
	int GetWidth() const { return m_width; }
	int GetHeight() const { return m_height; }

private:
	bool InitDuplication(int adapterIndex, int outputIndex);
	bool InitD3DDevice();
	void CaptureThread();
	void WindowCaptureThread();

	ComPtr<ID3D11Device> m_device;
	ComPtr<ID3D11DeviceContext> m_context;
	ComPtr<IDXGIOutputDuplication> m_duplication;

	// Window capture
	HWND m_captureWindow = NULL;
	bool m_windowMode = false;
	ComPtr<ID3D11Texture2D> m_stagingTexture;

	int m_width = 0;
	int m_height = 0;
	int m_targetFPS = 30;

	std::thread m_captureThread;
	std::atomic<bool> m_running{ false };

	FrameCallback m_frameCallback;

	uint32_t m_frameCount = 0;
};
