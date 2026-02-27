#include "H264Encoder.hpp"

#include <cstring>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>    // ICodecAPI
#include <codecapi.h>
#include <d3d11_1.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "evr.lib")

// {H264} MFVideoFormat constants
static const GUID MFVideoFormat_H264_LOCAL = { 0x34363248, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID MFVideoFormat_NV12_LOCAL = { 0x3231564E, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

H264Encoder::H264Encoder()
{
}

H264Encoder::~H264Encoder()
{
	Shutdown();
}

bool H264Encoder::Init(const Config& config, ID3D11Device* device)
{
	m_config = config;
	m_sampleDuration = 10000000ULL / config.fps; // 100ns units

	HRESULT hr = MFStartup(MF_VERSION);
	if (FAILED(hr))
		return false;

	if (device)
	{
		m_device = device;
		device->GetImmediateContext(m_deviceContext.GetAddressOf());
	}

	if (!CreateEncoder())
		return false;

	if (!ConfigureEncoder())
		return false;

	if (m_device && m_useHardware)
	{
		if (!SetupDXGIManager())
			return false;

		if (!SetupColorConverter())
			return false;
	}

	hr = m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	if (FAILED(hr))
		return false;

	hr = m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	if (FAILED(hr))
		return false;

	m_initialized = true;
	return true;
}

void H264Encoder::Shutdown()
{
	if (m_encoder)
	{
		m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		m_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
	}

	m_videoProcessor.Reset();
	m_videoProcessorEnum.Reset();
	m_videoContext.Reset();
	m_videoDevice.Reset();
	m_nv12Texture.Reset();
	m_encoder.Reset();
	m_deviceManager.Reset();
	m_deviceContext.Reset();
	m_device.Reset();

	if (m_initialized)
	{
		MFShutdown();
		m_initialized = false;
	}
}

bool H264Encoder::CreateEncoder()
{
	// Find an H.264 encoder MFT
	MFT_REGISTER_TYPE_INFO outputType = { MFMediaType_Video, MFVideoFormat_H264_LOCAL };

	IMFActivate** ppActivate = nullptr;
	UINT32 count = 0;

	// Try hardware first
	HRESULT hr = MFTEnumEx(
		MFT_CATEGORY_VIDEO_ENCODER,
		MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
		nullptr,
		&outputType,
		&ppActivate,
		&count
	);

	if (SUCCEEDED(hr) && count > 0)
	{
		hr = ppActivate[0]->ActivateObject(__uuidof(IMFTransform), (void**)m_encoder.GetAddressOf());
		m_useHardware = SUCCEEDED(hr);

		for (UINT32 i = 0; i < count; i++)
			ppActivate[i]->Release();
		CoTaskMemFree(ppActivate);

		if (m_useHardware)
			return true;
	}

	// Fallback to software
	hr = MFTEnumEx(
		MFT_CATEGORY_VIDEO_ENCODER,
		MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
		nullptr,
		&outputType,
		&ppActivate,
		&count
	);

	if (FAILED(hr) || count == 0)
		return false;

	hr = ppActivate[0]->ActivateObject(__uuidof(IMFTransform), (void**)m_encoder.GetAddressOf());

	for (UINT32 i = 0; i < count; i++)
		ppActivate[i]->Release();
	CoTaskMemFree(ppActivate);

	return SUCCEEDED(hr);
}

bool H264Encoder::ConfigureEncoder()
{
	HRESULT hr;

	// Set output type first (H.264)
	ComPtr<IMFMediaType> outputType;
	hr = MFCreateMediaType(outputType.GetAddressOf());
	if (FAILED(hr)) return false;

	outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264_LOCAL);
	outputType->SetUINT32(MF_MT_AVG_BITRATE, m_config.bitrate);
	MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, m_config.width, m_config.height);
	MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, m_config.fps, 1);
	outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);

	hr = m_encoder->SetOutputType(0, outputType.Get(), 0);
	if (FAILED(hr)) return false;

	// Set input type (NV12)
	ComPtr<IMFMediaType> inputType;
	hr = MFCreateMediaType(inputType.GetAddressOf());
	if (FAILED(hr)) return false;

	inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12_LOCAL);
	MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, m_config.width, m_config.height);
	MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, m_config.fps, 1);
	inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

	hr = m_encoder->SetInputType(0, inputType.Get(), 0);
	if (FAILED(hr)) return false;

	// Configure low latency via ICodecAPI
	ComPtr<ICodecAPI> codecAPI;
	hr = m_encoder.As(&codecAPI);
	if (SUCCEEDED(hr))
	{
		VARIANT val;

		// Low latency mode (critical for real-time streaming)
		VariantInit(&val);
		val.vt = VT_BOOL;
		val.boolVal = VARIANT_TRUE;
		codecAPI->SetValue(&CODECAPI_AVLowLatencyMode, &val);

		// CBR rate control
		VariantInit(&val);
		val.vt = VT_UI4;
		val.ulVal = eAVEncCommonRateControlMode_CBR;
		codecAPI->SetValue(&CODECAPI_AVEncCommonRateControlMode, &val);

		// GOP size
		VariantInit(&val);
		val.vt = VT_UI4;
		val.ulVal = m_config.keyframeInterval;
		codecAPI->SetValue(&CODECAPI_AVEncMPVGOPSize, &val);
	}

	return true;
}

bool H264Encoder::SetupDXGIManager()
{
	HRESULT hr = MFCreateDXGIDeviceManager(&m_resetToken, m_deviceManager.GetAddressOf());
	if (FAILED(hr))
		return false;

	hr = m_deviceManager->ResetDevice(m_device.Get(), m_resetToken);
	if (FAILED(hr))
		return false;

	// Set the device manager on the encoder
	hr = m_encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m_deviceManager.Get());
	if (FAILED(hr))
	{
		// Hardware encoder doesn't support D3D manager — fall back to software path
		m_useHardware = false;
		return true;
	}

	return true;
}

bool H264Encoder::SetupColorConverter()
{
	HRESULT hr;

	// Get D3D11 video device
	hr = m_device.As(&m_videoDevice);
	if (FAILED(hr)) return false;

	hr = m_deviceContext.As(&m_videoContext);
	if (FAILED(hr)) return false;

	// Create video processor enumerator
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
	contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	contentDesc.InputWidth = m_config.width;
	contentDesc.InputHeight = m_config.height;
	contentDesc.OutputWidth = m_config.width;
	contentDesc.OutputHeight = m_config.height;
	contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

	hr = m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, m_videoProcessorEnum.GetAddressOf());
	if (FAILED(hr)) return false;

	hr = m_videoDevice->CreateVideoProcessor(m_videoProcessorEnum.Get(), 0, m_videoProcessor.GetAddressOf());
	if (FAILED(hr)) return false;

	// Create NV12 texture for color-converted output
	D3D11_TEXTURE2D_DESC nv12Desc = {};
	nv12Desc.Width = m_config.width;
	nv12Desc.Height = m_config.height;
	nv12Desc.MipLevels = 1;
	nv12Desc.ArraySize = 1;
	nv12Desc.Format = DXGI_FORMAT_NV12;
	nv12Desc.SampleDesc.Count = 1;
	nv12Desc.Usage = D3D11_USAGE_DEFAULT;
	nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET;

	hr = m_device->CreateTexture2D(&nv12Desc, nullptr, m_nv12Texture.GetAddressOf());
	if (FAILED(hr)) return false;

	return true;
}

bool H264Encoder::Encode(ID3D11Texture2D* inputTexture, std::vector<uint8_t>& outputNALs)
{
	if (!m_initialized || !m_encoder)
		return false;

	outputNALs.clear();

	HRESULT hr;
	ComPtr<ID3D11Texture2D> encoderInput;

	if (m_videoProcessor && m_nv12Texture)
	{
		// GPU color conversion: BGRA -> NV12
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
		inputViewDesc.FourCC = 0;
		inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		inputViewDesc.Texture2D.MipSlice = 0;

		ComPtr<ID3D11VideoProcessorInputView> inputView;
		hr = m_videoDevice->CreateVideoProcessorInputView(
			inputTexture, m_videoProcessorEnum.Get(), &inputViewDesc, inputView.GetAddressOf());
		if (FAILED(hr)) return false;

		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
		outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

		ComPtr<ID3D11VideoProcessorOutputView> outputView;
		hr = m_videoDevice->CreateVideoProcessorOutputView(
			m_nv12Texture.Get(), m_videoProcessorEnum.Get(), &outputViewDesc, outputView.GetAddressOf());
		if (FAILED(hr)) return false;

		D3D11_VIDEO_PROCESSOR_STREAM streamData = {};
		streamData.Enable = TRUE;
		streamData.pInputSurface = inputView.Get();

		hr = m_videoContext->VideoProcessorBlt(
			m_videoProcessor.Get(), outputView.Get(), 0, 1, &streamData);
		if (FAILED(hr)) return false;

		encoderInput = m_nv12Texture;
	}
	else
	{
		// Software path — need to copy texture to a staging texture and convert manually
		// For now, just pass the texture through (software MFT may accept BGRA)
		encoderInput = inputTexture;
	}

	// Create MF sample from texture
	ComPtr<IMFSample> inputSample;
	hr = MFCreateSample(inputSample.GetAddressOf());
	if (FAILED(hr)) return false;

	ComPtr<IMFMediaBuffer> inputBuffer;
	hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), encoderInput.Get(), 0, FALSE, inputBuffer.GetAddressOf());
	if (FAILED(hr))
	{
		// Fallback: read texture to system memory
		D3D11_TEXTURE2D_DESC desc;
		inputTexture->GetDesc(&desc);

		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags = 0;

		ComPtr<ID3D11Texture2D> stagingTex;
		hr = m_device->CreateTexture2D(&stagingDesc, nullptr, stagingTex.GetAddressOf());
		if (FAILED(hr)) return false;

		m_deviceContext->CopyResource(stagingTex.Get(), inputTexture);

		D3D11_MAPPED_SUBRESOURCE mapped;
		hr = m_deviceContext->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
		if (FAILED(hr)) return false;

		DWORD bufferSize = mapped.RowPitch * desc.Height;
		hr = MFCreateMemoryBuffer(bufferSize, inputBuffer.GetAddressOf());
		if (FAILED(hr))
		{
			m_deviceContext->Unmap(stagingTex.Get(), 0);
			return false;
		}

		BYTE* bufferData = nullptr;
		inputBuffer->Lock(&bufferData, nullptr, nullptr);
		for (UINT row = 0; row < desc.Height; row++)
		{
			memcpy(bufferData + row * mapped.RowPitch,
			       (BYTE*)mapped.pData + row * mapped.RowPitch,
			       mapped.RowPitch);
		}
		inputBuffer->Unlock();
		inputBuffer->SetCurrentLength(bufferSize);

		m_deviceContext->Unmap(stagingTex.Get(), 0);
	}

	inputSample->AddBuffer(inputBuffer.Get());
	inputSample->SetSampleTime(m_sampleTime);
	inputSample->SetSampleDuration(m_sampleDuration);
	m_sampleTime += m_sampleDuration;

	// Feed to encoder
	hr = m_encoder->ProcessInput(0, inputSample.Get(), 0);
	if (FAILED(hr))
		return false;

	// Get output
	MFT_OUTPUT_DATA_BUFFER outputData = {};
	MFT_OUTPUT_STREAM_INFO streamInfo = {};
	m_encoder->GetOutputStreamInfo(0, &streamInfo);

	bool needSample = !(streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));

	ComPtr<IMFSample> outputSample;
	if (needSample)
	{
		hr = MFCreateSample(outputSample.GetAddressOf());
		if (FAILED(hr)) return false;

		ComPtr<IMFMediaBuffer> outBuf;
		hr = MFCreateMemoryBuffer(m_config.width * m_config.height * 2, outBuf.GetAddressOf());
		if (FAILED(hr)) return false;

		outputSample->AddBuffer(outBuf.Get());
		outputData.pSample = outputSample.Get();
	}

	DWORD status = 0;
	hr = m_encoder->ProcessOutput(0, 1, &outputData, &status);

	if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		return true; // No output yet, but no error

	if (FAILED(hr))
		return false;

	// Extract NAL data from output sample
	IMFSample* pOutSample = outputData.pSample;
	if (!pOutSample)
		return true;

	ComPtr<IMFMediaBuffer> outBuffer;
	hr = pOutSample->ConvertToContiguousBuffer(outBuffer.GetAddressOf());
	if (FAILED(hr)) return false;

	BYTE* outData = nullptr;
	DWORD outLen = 0;
	hr = outBuffer->Lock(&outData, nullptr, &outLen);
	if (FAILED(hr)) return false;

	outputNALs.assign(outData, outData + outLen);
	outBuffer->Unlock();

	// Release events if any
	if (outputData.pEvents)
		outputData.pEvents->Release();

	// If the MFT provided its own sample and we didn't provide one, don't release
	if (!needSample && outputData.pSample)
		outputData.pSample->Release();

	return true;
}

void H264Encoder::RequestKeyframe()
{
	if (!m_encoder)
		return;

	ComPtr<ICodecAPI> codecAPI;
	HRESULT hr = m_encoder.As(&codecAPI);
	if (SUCCEEDED(hr))
	{
		VARIANT val;
		VariantInit(&val);
		val.vt = VT_UI4;
		val.ulVal = 1;
		codecAPI->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &val);
	}
}
