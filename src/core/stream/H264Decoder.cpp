#include "H264Decoder.hpp"

#include <cstring>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

static const GUID MFVideoFormat_H264_LOCAL_D = { 0x34363248, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID MFVideoFormat_NV12_LOCAL_D = { 0x3231564E, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

H264Decoder::H264Decoder()
{
}

H264Decoder::~H264Decoder()
{
	Shutdown();
}

bool H264Decoder::Init(int width, int height)
{
	m_width = width;
	m_height = height;

	HRESULT hr = MFStartup(MF_VERSION);
	if (FAILED(hr))
		return false;

	if (!CreateDecoder())
		return false;

	if (!ConfigureDecoder())
		return false;

	m_initialized = true;
	return true;
}

void H264Decoder::Shutdown()
{
	if (m_decoder)
	{
		if (m_streamStarted)
		{
			m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
			m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
		}
		m_decoder.Reset();
	}

	if (m_initialized)
	{
		MFShutdown();
		m_initialized = false;
	}

	m_streamStarted = false;
}

bool H264Decoder::CreateDecoder()
{
	MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Video, MFVideoFormat_H264_LOCAL_D };

	IMFActivate** ppActivate = nullptr;
	UINT32 count = 0;

	// Try hardware decoder first
	HRESULT hr = MFTEnumEx(
		MFT_CATEGORY_VIDEO_DECODER,
		MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
		&inputType,
		nullptr,
		&ppActivate,
		&count);

	if (SUCCEEDED(hr) && count > 0)
	{
		hr = ppActivate[0]->ActivateObject(__uuidof(IMFTransform), (void**)m_decoder.GetAddressOf());

		for (UINT32 i = 0; i < count; i++)
			ppActivate[i]->Release();
		CoTaskMemFree(ppActivate);

		if (SUCCEEDED(hr))
			return true;
	}

	// Fallback to software decoder
	hr = MFTEnumEx(
		MFT_CATEGORY_VIDEO_DECODER,
		MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
		&inputType,
		nullptr,
		&ppActivate,
		&count);

	if (FAILED(hr) || count == 0)
		return false;

	hr = ppActivate[0]->ActivateObject(__uuidof(IMFTransform), (void**)m_decoder.GetAddressOf());

	for (UINT32 i = 0; i < count; i++)
		ppActivate[i]->Release();
	CoTaskMemFree(ppActivate);

	return SUCCEEDED(hr);
}

bool H264Decoder::ConfigureDecoder()
{
	HRESULT hr;

	// Set input type (H.264)
	ComPtr<IMFMediaType> inputType;
	hr = MFCreateMediaType(inputType.GetAddressOf());
	if (FAILED(hr)) return false;

	inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264_LOCAL_D);
	MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);

	hr = m_decoder->SetInputType(0, inputType.Get(), 0);
	if (FAILED(hr)) return false;

	// Find an output type that gives us NV12 or RGB32
	// Try to set NV12 output first
	for (DWORD i = 0; ; i++)
	{
		ComPtr<IMFMediaType> outputType;
		hr = m_decoder->GetOutputAvailableType(0, i, outputType.GetAddressOf());
		if (FAILED(hr))
			break;

		GUID subtype;
		outputType->GetGUID(MF_MT_SUBTYPE, &subtype);

		// Prefer NV12 — it's the most common decoder output
		if (subtype == MFVideoFormat_NV12_LOCAL_D || subtype == MFVideoFormat_NV12)
		{
			hr = m_decoder->SetOutputType(0, outputType.Get(), 0);
			if (SUCCEEDED(hr))
				return true;
		}
	}

	// If NV12 not available, accept whatever the decoder offers
	ComPtr<IMFMediaType> outputType;
	hr = m_decoder->GetOutputAvailableType(0, 0, outputType.GetAddressOf());
	if (FAILED(hr)) return false;

	hr = m_decoder->SetOutputType(0, outputType.Get(), 0);
	return SUCCEEDED(hr);
}

bool H264Decoder::Decode(const uint8_t* h264Data, size_t len,
                          std::vector<uint8_t>& outPixels, int& outWidth, int& outHeight)
{
	if (!m_initialized || !m_decoder || len == 0)
		return false;

	HRESULT hr;

	if (!m_streamStarted)
	{
		m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
		m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
		m_streamStarted = true;
	}

	// Create input sample
	ComPtr<IMFSample> inputSample;
	hr = MFCreateSample(inputSample.GetAddressOf());
	if (FAILED(hr)) return false;

	ComPtr<IMFMediaBuffer> inputBuffer;
	hr = MFCreateMemoryBuffer((DWORD)len, inputBuffer.GetAddressOf());
	if (FAILED(hr)) return false;

	BYTE* bufData = nullptr;
	inputBuffer->Lock(&bufData, nullptr, nullptr);
	std::memcpy(bufData, h264Data, len);
	inputBuffer->Unlock();
	inputBuffer->SetCurrentLength((DWORD)len);

	inputSample->AddBuffer(inputBuffer.Get());

	hr = m_decoder->ProcessInput(0, inputSample.Get(), 0);
	if (FAILED(hr))
		return false;

	// Try to get output
	MFT_OUTPUT_DATA_BUFFER outputData = {};
	MFT_OUTPUT_STREAM_INFO streamInfo = {};
	m_decoder->GetOutputStreamInfo(0, &streamInfo);

	bool needSample = !(streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));

	ComPtr<IMFSample> outputSample;
	if (needSample)
	{
		hr = MFCreateSample(outputSample.GetAddressOf());
		if (FAILED(hr)) return false;

		DWORD outBufSize = streamInfo.cbSize;
		if (outBufSize == 0)
			outBufSize = m_width * m_height * 4; // max BGRA

		ComPtr<IMFMediaBuffer> outBuf;
		hr = MFCreateMemoryBuffer(outBufSize, outBuf.GetAddressOf());
		if (FAILED(hr)) return false;

		outputSample->AddBuffer(outBuf.Get());
		outputData.pSample = outputSample.Get();
	}

	DWORD status = 0;
	hr = m_decoder->ProcessOutput(0, 1, &outputData, &status);

	if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		return false; // No output yet

	if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
	{
		// Output type changed — reconfigure
		ComPtr<IMFMediaType> newOutputType;
		hr = m_decoder->GetOutputAvailableType(0, 0, newOutputType.GetAddressOf());
		if (SUCCEEDED(hr))
		{
			m_decoder->SetOutputType(0, newOutputType.Get(), 0);

			// Update dimensions
			UINT32 w, h;
			if (SUCCEEDED(MFGetAttributeSize(newOutputType.Get(), MF_MT_FRAME_SIZE, &w, &h)))
			{
				m_width = w;
				m_height = h;
			}
		}
		return false;
	}

	if (FAILED(hr))
		return false;

	// Extract decoded frame
	IMFSample* pOutSample = outputData.pSample;
	if (!pOutSample)
		return false;

	ComPtr<IMFMediaBuffer> outBuffer;
	hr = pOutSample->ConvertToContiguousBuffer(outBuffer.GetAddressOf());
	if (FAILED(hr)) return false;

	BYTE* outData = nullptr;
	DWORD outLen = 0;
	hr = outBuffer->Lock(&outData, nullptr, &outLen);
	if (FAILED(hr)) return false;

	outWidth = m_width;
	outHeight = m_height;

	// Get current output subtype to know format
	ComPtr<IMFMediaType> curOutputType;
	m_decoder->GetOutputCurrentType(0, curOutputType.GetAddressOf());

	GUID subtype = {};
	if (curOutputType)
		curOutputType->GetGUID(MF_MT_SUBTYPE, &subtype);

	if (subtype == MFVideoFormat_NV12_LOCAL_D || subtype == MFVideoFormat_NV12)
	{
		// Convert NV12 to BGRA
		int frameSize = outWidth * outHeight;
		outPixels.resize(outWidth * outHeight * 4);

		const uint8_t* yPlane = outData;
		const uint8_t* uvPlane = outData + frameSize;

		// Get stride from buffer (may differ from width)
		UINT32 stride = outWidth;
		ComPtr<IMFMediaType> outType;
		m_decoder->GetOutputCurrentType(0, outType.GetAddressOf());
		if (outType)
		{
			UINT32 defaultStride = 0;
			if (SUCCEEDED(outType->GetUINT32(MF_MT_DEFAULT_STRIDE, &defaultStride)))
				stride = defaultStride;
		}
		if (stride == 0) stride = outWidth;

		for (int y = 0; y < outHeight; y++)
		{
			for (int x = 0; x < outWidth; x++)
			{
				int yIdx = y * stride + x;
				int uvIdx = (y / 2) * stride + (x & ~1);

				uint8_t Y = yPlane[yIdx];
				uint8_t U = uvPlane[uvIdx];
				uint8_t V = uvPlane[uvIdx + 1];

				int C = Y - 16;
				int D = U - 128;
				int E = V - 128;

				int R = (298 * C + 409 * E + 128) >> 8;
				int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
				int B = (298 * C + 516 * D + 128) >> 8;

				if (R < 0) R = 0; if (R > 255) R = 255;
				if (G < 0) G = 0; if (G > 255) G = 255;
				if (B < 0) B = 0; if (B > 255) B = 255;

				int pixIdx = (y * outWidth + x) * 4;
				outPixels[pixIdx + 0] = (uint8_t)B;
				outPixels[pixIdx + 1] = (uint8_t)G;
				outPixels[pixIdx + 2] = (uint8_t)R;
				outPixels[pixIdx + 3] = 255;
			}
		}
	}
	else
	{
		// Assume BGRA or just copy raw
		outPixels.assign(outData, outData + outLen);
	}

	outBuffer->Unlock();

	if (outputData.pEvents)
		outputData.pEvents->Release();
	if (!needSample && outputData.pSample)
		outputData.pSample->Release();

	return true;
}
