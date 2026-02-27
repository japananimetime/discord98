#include "ScreenCapture.hpp"
#include <dxgi.h>
#include <chrono>
#include <thread>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x00000001
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

ScreenCapture::ScreenCapture()
{
}

ScreenCapture::~ScreenCapture()
{
	Shutdown();
}

bool ScreenCapture::Init(int adapterIndex, int outputIndex)
{
	m_windowMode = false;
	m_captureWindow = NULL;
	return InitDuplication(adapterIndex, outputIndex);
}

bool ScreenCapture::InitFromWindow(HWND hwnd)
{
	if (!IsWindow(hwnd))
		return false;

	m_windowMode = true;
	m_captureWindow = hwnd;

	RECT rc;
	GetClientRect(hwnd, &rc);
	m_width = rc.right - rc.left;
	m_height = rc.bottom - rc.top;

	if (m_width <= 0 || m_height <= 0)
		return false;

	return InitD3DDevice();
}

bool ScreenCapture::InitD3DDevice()
{
	D3D_FEATURE_LEVEL featureLevel;
	HRESULT hr = D3D11CreateDevice(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		nullptr, 0, D3D11_SDK_VERSION,
		m_device.GetAddressOf(), &featureLevel, m_context.GetAddressOf());
	return SUCCEEDED(hr);
}

void ScreenCapture::Shutdown()
{
	Stop();

	m_duplication.Reset();
	m_stagingTexture.Reset();
	m_context.Reset();
	m_device.Reset();
	m_width = 0;
	m_height = 0;
	m_windowMode = false;
	m_captureWindow = NULL;
}

bool ScreenCapture::InitDuplication(int adapterIndex, int outputIndex)
{
	// Create D3D11 device
	D3D_FEATURE_LEVEL featureLevel;
	HRESULT hr = D3D11CreateDevice(
		nullptr,                    // default adapter
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		0,                          // flags
		nullptr, 0,                 // feature levels (default)
		D3D11_SDK_VERSION,
		m_device.GetAddressOf(),
		&featureLevel,
		m_context.GetAddressOf()
	);

	if (FAILED(hr))
		return false;

	// Get DXGI device -> adapter -> output
	ComPtr<IDXGIDevice> dxgiDevice;
	hr = m_device.As(&dxgiDevice);
	if (FAILED(hr))
		return false;

	ComPtr<IDXGIAdapter> adapter;
	hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)adapter.GetAddressOf());
	if (FAILED(hr))
		return false;

	ComPtr<IDXGIOutput> output;
	hr = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
	if (FAILED(hr))
		return false;

	// Get output dimensions
	DXGI_OUTPUT_DESC outputDesc;
	output->GetDesc(&outputDesc);
	m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
	m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

	// Create desktop duplication
	ComPtr<IDXGIOutput1> output1;
	hr = output.As(&output1);
	if (FAILED(hr))
		return false;

	hr = output1->DuplicateOutput(m_device.Get(), m_duplication.GetAddressOf());
	if (FAILED(hr))
		return false;

	return true;
}

void ScreenCapture::Start(int targetFPS)
{
	if (m_running)
		return;

	m_targetFPS = targetFPS;
	m_running = true;
	m_frameCount = 0;

	if (m_windowMode)
		m_captureThread = std::thread(&ScreenCapture::WindowCaptureThread, this);
	else
		m_captureThread = std::thread(&ScreenCapture::CaptureThread, this);
}

void ScreenCapture::Stop()
{
	m_running = false;
	if (m_captureThread.joinable())
		m_captureThread.join();
}

void ScreenCapture::CaptureThread()
{
	auto startTime = std::chrono::steady_clock::now();

	while (m_running)
	{
		DXGI_OUTDUPL_FRAME_INFO frameInfo;
		ComPtr<IDXGIResource> desktopResource;

		// Try to acquire next frame with a short timeout
		HRESULT hr = m_duplication->AcquireNextFrame(100, &frameInfo, desktopResource.GetAddressOf());

		if (hr == DXGI_ERROR_ACCESS_LOST)
		{
			// Desktop switch (UAC, lock screen, etc.) — re-initialize
			m_duplication.Reset();
			if (!InitDuplication(0, 0))
			{
				// Failed to re-init, wait and retry
				std::this_thread::sleep_for(std::chrono::seconds(1));
				continue;
			}
			continue;
		}

		if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		{
			// No new frame — continue
			continue;
		}

		if (FAILED(hr))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		// Get the desktop texture
		ComPtr<ID3D11Texture2D> desktopTexture;
		hr = desktopResource.As(&desktopTexture);

		if (SUCCEEDED(hr) && m_frameCallback)
		{
			// Calculate RTP timestamp at 90kHz clock
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime);
			uint32_t timestamp90kHz = static_cast<uint32_t>((elapsed.count() * 90) / 1000);

			m_frameCallback(desktopTexture.Get(), m_width, m_height, timestamp90kHz);
			m_frameCount++;
		}

		m_duplication->ReleaseFrame();

		// Rate limiting — sleep to maintain target FPS
		auto frameEnd = std::chrono::steady_clock::now();
		auto frameElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - startTime);
		auto expectedMs = std::chrono::milliseconds(m_frameCount * 1000 / m_targetFPS);
		if (expectedMs > frameElapsed)
		{
			std::this_thread::sleep_for(expectedMs - frameElapsed);
		}
	}
}

void ScreenCapture::WindowCaptureThread()
{
	auto startTime = std::chrono::steady_clock::now();

	while (m_running)
	{
		if (!IsWindow(m_captureWindow))
		{
			// Window was closed
			m_running = false;
			break;
		}

		// Update window dimensions
		RECT rc;
		GetClientRect(m_captureWindow, &rc);
		int curW = rc.right - rc.left;
		int curH = rc.bottom - rc.top;

		if (curW <= 0 || curH <= 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		// Re-create staging texture if size changed
		if (curW != m_width || curH != m_height || !m_stagingTexture)
		{
			m_width = curW;
			m_height = curH;
			m_stagingTexture.Reset();

			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = m_width;
			desc.Height = m_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, m_stagingTexture.GetAddressOf());
			if (FAILED(hr))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
		}

		// Capture window content via GDI → D3D11 texture
		HDC hWinDC = GetDC(m_captureWindow);
		if (!hWinDC)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		HDC hMemDC = CreateCompatibleDC(hWinDC);
		HBITMAP hBmp = CreateCompatibleBitmap(hWinDC, m_width, m_height);
		HGDIOBJ old = SelectObject(hMemDC, hBmp);

		// Use PrintWindow for better results with layered/occluded windows
		if (!PrintWindow(m_captureWindow, hMemDC, PW_CLIENTONLY | PW_RENDERFULLCONTENT))
		{
			BitBlt(hMemDC, 0, 0, m_width, m_height, hWinDC, 0, 0, SRCCOPY);
		}

		SelectObject(hMemDC, old);
		ReleaseDC(m_captureWindow, hWinDC);

		// Get bitmap bits
		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = m_width;
		bmi.bmiHeader.biHeight = -m_height; // top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		std::vector<uint8_t> pixels(m_width * m_height * 4);
		GetDIBits(hMemDC, hBmp, 0, m_height, pixels.data(), &bmi, DIB_RGB_COLORS);

		DeleteObject(hBmp);
		DeleteDC(hMemDC);

		// Upload to D3D11 texture
		D3D11_BOX box = { 0, 0, 0, (UINT)m_width, (UINT)m_height, 1 };
		m_context->UpdateSubresource(m_stagingTexture.Get(), 0, &box, pixels.data(), m_width * 4, 0);

		if (m_frameCallback)
		{
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime);
			uint32_t timestamp90kHz = static_cast<uint32_t>((elapsed.count() * 90) / 1000);

			m_frameCallback(m_stagingTexture.Get(), m_width, m_height, timestamp90kHz);
			m_frameCount++;
		}

		// Rate limiting
		auto frameEnd = std::chrono::steady_clock::now();
		auto frameElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - startTime);
		auto expectedMs = std::chrono::milliseconds(m_frameCount * 1000 / m_targetFPS);
		if (expectedMs > frameElapsed)
		{
			std::this_thread::sleep_for(expectedMs - frameElapsed);
		}
	}
}
