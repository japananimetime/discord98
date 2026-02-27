#pragma once

#include <cstdint>
#include <array>
#include <vector>

namespace dv { class UDPSocket; }

class VideoRTPSender
{
public:
	void Init(dv::UDPSocket* udp, uint32_t videoSSRC,
	          const std::array<uint8_t, 32>& secretKey);

	// Send a complete H.264 access unit (one or more NAL units separated by start codes)
	void SendFrame(const uint8_t* h264Data, size_t len, uint32_t timestamp);

	void SetPayloadType(uint8_t pt) { m_payloadType = pt; }

private:
	// Send a single NAL unit (may fragment into FU-A if too large)
	void SendNALUnit(const uint8_t* nal, size_t len, uint32_t timestamp, bool lastNAL);

	// Send FU-A fragmented packets (RFC 6184) for NALUs > MTU
	void SendFUA(const uint8_t* nal, size_t len, uint32_t timestamp, bool lastNAL);

	// Send a single RTP packet with encryption
	void SendRTPPacket(const uint8_t* payload, size_t len, uint32_t timestamp, bool marker);

	dv::UDPSocket* m_udp = nullptr;
	uint32_t m_videoSSRC = 0;
	std::array<uint8_t, 32> m_secretKey{};

	uint16_t m_sequence = 0;
	uint32_t m_nonce = 0;
	uint8_t m_payloadType = 101; // H.264

	static constexpr size_t MAX_RTP_PAYLOAD = 1200; // MTU-safe
};
