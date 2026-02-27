#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace dv { class UDPSocket; }
struct OpusEncoder;
struct IMMDevice;
struct IAudioClient;
struct IAudioCaptureClient;

class LoopbackCapture
{
public:
	LoopbackCapture();
	~LoopbackCapture();

	// Initialize with the stream's UDP socket, audio SSRC, and secret key
	bool Init(dv::UDPSocket* udp, uint32_t audioSSRC,
	          const std::array<uint8_t, 32>& secretKey);
	void Shutdown();

	void Start();
	void Stop();

	bool IsRunning() const { return m_running; }

	void SetGain(float gain) { m_gain = gain; }

private:
	void CaptureThread();
	void SendOpusPacket(const uint8_t* data, int size, uint32_t timestamp);

	dv::UDPSocket* m_udp = nullptr;
	uint32_t m_audioSSRC = 0;
	std::array<uint8_t, 32> m_secretKey{};

	// WASAPI loopback
	IMMDevice* m_device = nullptr;
	IAudioClient* m_audioClient = nullptr;
	IAudioCaptureClient* m_captureClient = nullptr;

	// Opus encoder for loopback audio
	OpusEncoder* m_encoder = nullptr;

	// Threading
	std::thread m_thread;
	std::atomic<bool> m_running{ false };

	// RTP state
	uint16_t m_sequence = 0;
	uint32_t m_nonce = 0;
	uint32_t m_rtpTimestamp = 0;

	// Audio state
	std::atomic<float> m_gain{ 1.0f };
	std::vector<int16_t> m_resampleBuffer;
};
