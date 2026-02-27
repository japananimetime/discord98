#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

class VideoRTPReceiver
{
public:
	void Init(uint32_t videoSSRC, const std::array<uint8_t, 32>& secretKey);

	// Feed a raw UDP packet (encrypted RTP)
	void Feed(const std::vector<uint8_t>& data);

	// Callback: called when a complete H.264 access unit is reassembled
	using FrameCallback = std::function<void(const uint8_t* h264Data, size_t len, uint32_t timestamp)>;
	void SetFrameCallback(FrameCallback cb) { m_frameCallback = std::move(cb); }

private:
	bool DecryptPacket(const uint8_t* data, size_t len,
	                   uint8_t& payloadType, uint16_t& seq, uint32_t& timestamp, uint32_t& ssrc,
	                   std::vector<uint8_t>& payload);

	void ProcessPayload(const uint8_t* payload, size_t len, uint16_t seq, uint32_t timestamp);
	void FlushFrame(uint32_t timestamp);

	uint32_t m_videoSSRC = 0;
	std::array<uint8_t, 32> m_secretKey{};

	// Reassembly state
	struct Fragment {
		uint16_t seq;
		std::vector<uint8_t> data;
	};

	uint32_t m_currentTimestamp = 0;
	bool m_hasTimestamp = false;

	// Accumulated NAL data for current frame
	std::vector<uint8_t> m_frameBuffer;

	// FU-A reassembly
	std::vector<uint8_t> m_fuaBuffer;
	bool m_fuaInProgress = false;

	FrameCallback m_frameCallback;
	std::mutex m_mutex;
};
