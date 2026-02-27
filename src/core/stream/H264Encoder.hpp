#pragma once

#include <vector>
#include <cstdint>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class H264Encoder
{
public:
	struct Config
	{
		int width = 1280;
		int height = 720;
		int fps = 30;
		int bitrate = 2500000; // 2.5 Mbps
		int keyframeInterval = 60;
	};

	H264Encoder();
	~H264Encoder();

	bool Init(const Config& config, ID3D11Device* device = nullptr);
	void Shutdown();

	// Encode a D3D11 texture to H.264 NAL units
	bool Encode(ID3D11Texture2D* inputTexture, std::vector<uint8_t>& outputNALs);

	void RequestKeyframe();

private:
	bool CreateEncoder();
	bool ConfigureEncoder();
	bool SetupDXGIManager();
	bool SetupColorConverter();

	ComPtr<IMFTransform> m_encoder;
	ComPtr<IMFDXGIDeviceManager> m_deviceManager;
	ComPtr<ID3D11Device> m_device;
	ComPtr<ID3D11DeviceContext> m_deviceContext;

	// Video processor for BGRA -> NV12 conversion
	ComPtr<ID3D11VideoDevice> m_videoDevice;
	ComPtr<ID3D11VideoContext> m_videoContext;
	ComPtr<ID3D11VideoProcessor> m_videoProcessor;
	ComPtr<ID3D11VideoProcessorEnumerator> m_videoProcessorEnum;

	// NV12 staging texture for encoder input
	ComPtr<ID3D11Texture2D> m_nv12Texture;

	Config m_config;
	UINT m_resetToken = 0;
	bool m_initialized = false;
	bool m_useHardware = false;
	uint64_t m_sampleTime = 0;
	uint64_t m_sampleDuration = 0;
};
