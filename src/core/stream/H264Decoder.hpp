#pragma once

#include <vector>
#include <cstdint>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class H264Decoder
{
public:
	H264Decoder();
	~H264Decoder();

	bool Init(int width = 1280, int height = 720);
	void Shutdown();

	// Decode H.264 NAL units (with start codes) to BGRA pixel data
	// Returns true if a frame was decoded. Output pixels are BGRA, top-down.
	bool Decode(const uint8_t* h264Data, size_t len,
	            std::vector<uint8_t>& outPixels, int& outWidth, int& outHeight);

private:
	bool CreateDecoder();
	bool ConfigureDecoder();

	ComPtr<IMFTransform> m_decoder;
	int m_width = 0;
	int m_height = 0;
	bool m_initialized = false;
	bool m_streamStarted = false;
};
