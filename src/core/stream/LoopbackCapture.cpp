#include <winsock2.h>
#include <ws2tcpip.h>

#include "LoopbackCapture.hpp"
#include <udp_socket.h>
#include <sodium.h>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <opus/opus.h>
#include <cstring>

#pragma comment(lib, "ole32.lib")

// Opus settings matching Discord voice
static const int OPUS_SAMPLE_RATE = 48000;
static const int OPUS_CHANNELS = 2;
static const int OPUS_FRAME_MS = 20;
static const int OPUS_FRAME_SAMPLES = OPUS_SAMPLE_RATE * OPUS_FRAME_MS / 1000; // 960
static const int OPUS_MAX_PACKET = 1275;

LoopbackCapture::LoopbackCapture()
{
}

LoopbackCapture::~LoopbackCapture()
{
	Shutdown();
}

bool LoopbackCapture::Init(dv::UDPSocket* udp, uint32_t audioSSRC,
                            const std::array<uint8_t, 32>& secretKey)
{
	m_udp = udp;
	m_audioSSRC = audioSSRC;
	m_secretKey = secretKey;
	m_sequence = 0;
	m_nonce = 0;
	m_rtpTimestamp = 0;

	// Initialize COM for this thread (if not already)
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	// Get default audio render device for loopback
	IMMDeviceEnumerator* enumerator = nullptr;
	HRESULT hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), (void**)&enumerator);

	if (FAILED(hr))
		return false;

	hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
	enumerator->Release();

	if (FAILED(hr))
		return false;

	// Activate audio client
	hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
	if (FAILED(hr))
		return false;

	// Get mix format
	WAVEFORMATEX* mixFormat = nullptr;
	hr = m_audioClient->GetMixFormat(&mixFormat);
	if (FAILED(hr))
		return false;

	// Initialize in loopback mode
	hr = m_audioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED,
		AUDCLNT_STREAMFLAGS_LOOPBACK,
		10000000,  // 1 second buffer
		0,
		mixFormat,
		nullptr);

	CoTaskMemFree(mixFormat);

	if (FAILED(hr))
		return false;

	// Get capture client
	hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
	if (FAILED(hr))
		return false;

	// Create Opus encoder for stereo audio
	int opusError;
	m_encoder = opus_encoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, OPUS_APPLICATION_AUDIO, &opusError);
	if (opusError != OPUS_OK || !m_encoder)
		return false;

	opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(128000));
	opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

	return true;
}

void LoopbackCapture::Shutdown()
{
	Stop();

	if (m_encoder) {
		opus_encoder_destroy(m_encoder);
		m_encoder = nullptr;
	}

	if (m_captureClient) {
		m_captureClient->Release();
		m_captureClient = nullptr;
	}

	if (m_audioClient) {
		m_audioClient->Release();
		m_audioClient = nullptr;
	}

	if (m_device) {
		m_device->Release();
		m_device = nullptr;
	}
}

void LoopbackCapture::Start()
{
	if (m_running)
		return;

	if (!m_audioClient)
		return;

	HRESULT hr = m_audioClient->Start();
	if (FAILED(hr))
		return;

	m_running = true;
	m_thread = std::thread(&LoopbackCapture::CaptureThread, this);
}

void LoopbackCapture::Stop()
{
	m_running = false;

	if (m_thread.joinable())
		m_thread.join();

	if (m_audioClient)
		m_audioClient->Stop();
}

void LoopbackCapture::CaptureThread()
{
	// Get the mix format to know how to convert
	WAVEFORMATEX* mixFormat = nullptr;
	m_audioClient->GetMixFormat(&mixFormat);

	if (!mixFormat)
	{
		m_running = false;
		return;
	}

	int srcRate = mixFormat->nSamplesPerSec;
	int srcChannels = mixFormat->nChannels;
	int srcBits = mixFormat->wBitsPerSample;
	bool isFloat = false;

	if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
	{
		WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)mixFormat;
		isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
	}
	else if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
	{
		isFloat = true;
	}

	// Accumulation buffer for building 20ms frames
	std::vector<int16_t> accumBuffer;
	int targetFrameSamples = OPUS_FRAME_SAMPLES * OPUS_CHANNELS; // 960 * 2 = 1920 samples per frame

	std::vector<uint8_t> opusBuffer(OPUS_MAX_PACKET);

	while (m_running)
	{
		// Wait for data
		UINT32 packetLength = 0;
		HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);

		if (FAILED(hr))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		if (packetLength == 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		BYTE* data = nullptr;
		UINT32 framesAvailable = 0;
		DWORD flags = 0;

		hr = m_captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
		if (FAILED(hr))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
		float gain = m_gain.load();

		// Convert to 48kHz stereo int16
		int outSamples = framesAvailable;
		if (srcRate != OPUS_SAMPLE_RATE)
		{
			// Simple nearest-neighbor resample
			outSamples = (int)((int64_t)framesAvailable * OPUS_SAMPLE_RATE / srcRate);
		}

		int prevSize = (int)accumBuffer.size();
		accumBuffer.resize(prevSize + outSamples * OPUS_CHANNELS);

		if (silent)
		{
			std::memset(accumBuffer.data() + prevSize, 0, outSamples * OPUS_CHANNELS * sizeof(int16_t));
		}
		else
		{
			for (int i = 0; i < outSamples; i++)
			{
				// Map output sample to input sample (nearest-neighbor resample)
				int srcIdx = (srcRate != OPUS_SAMPLE_RATE) ?
					(int)((int64_t)i * srcRate / OPUS_SAMPLE_RATE) : i;
				if (srcIdx >= (int)framesAvailable) srcIdx = framesAvailable - 1;

				for (int ch = 0; ch < OPUS_CHANNELS; ch++)
				{
					float sample = 0.0f;
					int srcCh = (ch < srcChannels) ? ch : 0;

					if (isFloat && srcBits == 32)
					{
						float* fdata = (float*)data;
						sample = fdata[srcIdx * srcChannels + srcCh];
					}
					else if (srcBits == 16)
					{
						int16_t* sdata = (int16_t*)data;
						sample = sdata[srcIdx * srcChannels + srcCh] / 32768.0f;
					}
					else if (srcBits == 32 && !isFloat)
					{
						int32_t* idata = (int32_t*)data;
						sample = idata[srcIdx * srcChannels + srcCh] / 2147483648.0f;
					}

					sample *= gain;

					// Clamp and convert to int16
					if (sample > 1.0f) sample = 1.0f;
					if (sample < -1.0f) sample = -1.0f;
					accumBuffer[prevSize + i * OPUS_CHANNELS + ch] = (int16_t)(sample * 32767.0f);
				}
			}
		}

		m_captureClient->ReleaseBuffer(framesAvailable);

		// Encode and send complete 20ms frames
		while ((int)accumBuffer.size() >= targetFrameSamples)
		{
			int encoded = opus_encode(m_encoder, accumBuffer.data(),
				OPUS_FRAME_SAMPLES, opusBuffer.data(), OPUS_MAX_PACKET);

			if (encoded > 0)
			{
				SendOpusPacket(opusBuffer.data(), encoded, m_rtpTimestamp);
				m_rtpTimestamp += OPUS_FRAME_SAMPLES;
			}

			// Remove consumed samples
			accumBuffer.erase(accumBuffer.begin(), accumBuffer.begin() + targetFrameSamples);
		}
	}

	CoTaskMemFree(mixFormat);
}

void LoopbackCapture::SendOpusPacket(const uint8_t* data, int size, uint32_t timestamp)
{
	if (!m_udp || size <= 0)
		return;

	m_sequence++;
	m_nonce++;

	// Build RTP header (12 bytes) + encrypted payload + auth tag + 4-byte nonce
	const size_t encryptedSize = size + crypto_aead_xchacha20poly1305_ietf_ABYTES;
	std::vector<uint8_t> rtp(12 + encryptedSize + sizeof(uint32_t), 0);

	// RTP header
	rtp[0] = 0x80; // Version 2
	rtp[1] = 0x78; // Payload type 120 (Opus)
	rtp[2] = (m_sequence >> 8) & 0xFF;
	rtp[3] = (m_sequence >> 0) & 0xFF;
	rtp[4] = (timestamp >> 24) & 0xFF;
	rtp[5] = (timestamp >> 16) & 0xFF;
	rtp[6] = (timestamp >> 8) & 0xFF;
	rtp[7] = (timestamp >> 0) & 0xFF;
	rtp[8] = (m_audioSSRC >> 24) & 0xFF;
	rtp[9] = (m_audioSSRC >> 16) & 0xFF;
	rtp[10] = (m_audioSSRC >> 8) & 0xFF;
	rtp[11] = (m_audioSSRC >> 0) & 0xFF;

	// Nonce
	std::array<uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonceBytes{};
	std::memcpy(nonceBytes.data(), &m_nonce, sizeof(uint32_t));

	// Encrypt
	unsigned long long ciphertextLen;
	crypto_aead_xchacha20poly1305_ietf_encrypt(
		rtp.data() + 12, &ciphertextLen,
		data, size,
		rtp.data(), 12,
		nullptr,
		nonceBytes.data(),
		m_secretKey.data());

	// Append nonce
	rtp.resize(12 + static_cast<size_t>(ciphertextLen) + sizeof(uint32_t));
	std::memcpy(rtp.data() + rtp.size() - sizeof(uint32_t), &m_nonce, sizeof(uint32_t));

	m_udp->Send(rtp.data(), rtp.size());
}
