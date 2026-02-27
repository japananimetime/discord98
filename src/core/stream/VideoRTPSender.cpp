#include <winsock2.h>
#include <ws2tcpip.h>

#include "VideoRTPSender.hpp"
#include <udp_socket.h>
#include <sodium.h>
#include <cstring>

void VideoRTPSender::Init(dv::UDPSocket* udp, uint32_t videoSSRC,
                           const std::array<uint8_t, 32>& secretKey)
{
	m_udp = udp;
	m_videoSSRC = videoSSRC;
	m_secretKey = secretKey;
	m_sequence = 0;
	m_nonce = 0;
}

void VideoRTPSender::SendFrame(const uint8_t* h264Data, size_t len, uint32_t timestamp)
{
	if (!m_udp || len == 0)
		return;

	// Parse H.264 bitstream: find NAL units separated by start codes (00 00 00 01 or 00 00 01)
	std::vector<std::pair<const uint8_t*, size_t>> nalUnits;

	size_t i = 0;
	while (i < len)
	{
		// Find start code
		size_t startCodeLen = 0;
		if (i + 3 < len && h264Data[i] == 0 && h264Data[i+1] == 0 && h264Data[i+2] == 0 && h264Data[i+3] == 1)
			startCodeLen = 4;
		else if (i + 2 < len && h264Data[i] == 0 && h264Data[i+1] == 0 && h264Data[i+2] == 1)
			startCodeLen = 3;

		if (startCodeLen > 0)
		{
			size_t nalStart = i + startCodeLen;

			// Find the end of this NAL (next start code or end of data)
			size_t nalEnd = len;
			for (size_t j = nalStart; j + 2 < len; j++)
			{
				if (h264Data[j] == 0 && h264Data[j+1] == 0 &&
				    (h264Data[j+2] == 1 || (j + 3 < len && h264Data[j+2] == 0 && h264Data[j+3] == 1)))
				{
					nalEnd = j;
					break;
				}
			}

			if (nalEnd > nalStart)
				nalUnits.push_back({ h264Data + nalStart, nalEnd - nalStart });

			i = nalEnd;
		}
		else
		{
			// If no start code at beginning, treat entire buffer as single NAL
			if (nalUnits.empty())
			{
				nalUnits.push_back({ h264Data, len });
				break;
			}
			i++;
		}
	}

	// Send each NAL unit
	for (size_t n = 0; n < nalUnits.size(); n++)
	{
		bool lastNAL = (n == nalUnits.size() - 1);
		SendNALUnit(nalUnits[n].first, nalUnits[n].second, timestamp, lastNAL);
	}
}

void VideoRTPSender::SendNALUnit(const uint8_t* nal, size_t len, uint32_t timestamp, bool lastNAL)
{
	if (len == 0)
		return;

	if (len <= MAX_RTP_PAYLOAD)
	{
		// Single NAL unit packet — send as-is
		SendRTPPacket(nal, len, timestamp, lastNAL);
	}
	else
	{
		// Fragment using FU-A (RFC 6184)
		SendFUA(nal, len, timestamp, lastNAL);
	}
}

void VideoRTPSender::SendFUA(const uint8_t* nal, size_t len, uint32_t timestamp, bool lastNAL)
{
	if (len < 2)
		return;

	// NAL header byte
	uint8_t nalHeader = nal[0];
	uint8_t nalType = nalHeader & 0x1F;
	uint8_t nri = nalHeader & 0x60; // forbidden_zero_bit is always 0

	// FU-A indicator: type = 28 (FU-A), NRI from original NAL
	uint8_t fuIndicator = nri | 28;

	// Skip the NAL header byte for fragmentation
	const uint8_t* payload = nal + 1;
	size_t remaining = len - 1;

	// Max fragment payload = MTU - 2 bytes (FU indicator + FU header)
	size_t maxFragPayload = MAX_RTP_PAYLOAD - 2;
	bool isFirst = true;

	while (remaining > 0)
	{
		size_t fragLen = (remaining > maxFragPayload) ? maxFragPayload : remaining;
		bool isLast = (fragLen == remaining);

		// FU header: S=start, E=end, R=0, Type=original NAL type
		uint8_t fuHeader = nalType;
		if (isFirst)
			fuHeader |= 0x80; // Start bit
		if (isLast)
			fuHeader |= 0x40; // End bit

		// Build FU-A packet: FU indicator + FU header + fragment
		std::vector<uint8_t> fuPacket(2 + fragLen);
		fuPacket[0] = fuIndicator;
		fuPacket[1] = fuHeader;
		std::memcpy(fuPacket.data() + 2, payload, fragLen);

		// Marker bit: set only on last packet of last NAL in access unit
		bool marker = isLast && lastNAL;
		SendRTPPacket(fuPacket.data(), fuPacket.size(), timestamp, marker);

		payload += fragLen;
		remaining -= fragLen;
		isFirst = false;
	}
}

void VideoRTPSender::SendRTPPacket(const uint8_t* payload, size_t len,
                                    uint32_t timestamp, bool marker)
{
	m_sequence++;
	m_nonce++;

	// Build RTP header (12 bytes) + encrypted payload + auth tag + 4-byte nonce
	const size_t encryptedSize = len + crypto_aead_xchacha20poly1305_ietf_ABYTES;
	std::vector<uint8_t> rtp(12 + encryptedSize + sizeof(uint32_t), 0);

	// RTP header
	rtp[0] = 0x80; // Version 2
	rtp[1] = m_payloadType;
	if (marker)
		rtp[1] |= 0x80; // Marker bit

	rtp[2] = (m_sequence >> 8) & 0xFF;
	rtp[3] = (m_sequence >> 0) & 0xFF;
	rtp[4] = (timestamp >> 24) & 0xFF;
	rtp[5] = (timestamp >> 16) & 0xFF;
	rtp[6] = (timestamp >> 8) & 0xFF;
	rtp[7] = (timestamp >> 0) & 0xFF;
	rtp[8] = (m_videoSSRC >> 24) & 0xFF;
	rtp[9] = (m_videoSSRC >> 16) & 0xFF;
	rtp[10] = (m_videoSSRC >> 8) & 0xFF;
	rtp[11] = (m_videoSSRC >> 0) & 0xFF;

	// Nonce: 4-byte counter in first 4 bytes, rest zeroed
	std::array<uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonceBytes{};
	std::memcpy(nonceBytes.data(), &m_nonce, sizeof(uint32_t));

	// Encrypt with AEAD XChaCha20-Poly1305
	// AAD = RTP header (12 bytes)
	unsigned long long ciphertextLen;
	crypto_aead_xchacha20poly1305_ietf_encrypt(
		rtp.data() + 12, &ciphertextLen,
		payload, len,
		rtp.data(), 12, // AAD = RTP header
		nullptr,
		nonceBytes.data(),
		m_secretKey.data());

	// Append 4-byte nonce counter at end
	rtp.resize(12 + static_cast<size_t>(ciphertextLen) + sizeof(uint32_t));
	std::memcpy(rtp.data() + rtp.size() - sizeof(uint32_t), &m_nonce, sizeof(uint32_t));

	m_udp->Send(rtp.data(), rtp.size());
}
