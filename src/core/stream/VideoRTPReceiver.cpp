#include <winsock2.h>
#include <ws2tcpip.h>

#include "VideoRTPReceiver.hpp"
#include <sodium.h>
#include <cstring>

void VideoRTPReceiver::Init(uint32_t videoSSRC, const std::array<uint8_t, 32>& secretKey)
{
	m_videoSSRC = videoSSRC;
	m_secretKey = secretKey;
	m_currentTimestamp = 0;
	m_hasTimestamp = false;
	m_frameBuffer.clear();
	m_fuaBuffer.clear();
	m_fuaInProgress = false;
}

void VideoRTPReceiver::Feed(const std::vector<uint8_t>& data)
{
	if (data.size() < 12)
		return;

	uint8_t payloadType;
	uint16_t seq;
	uint32_t timestamp, ssrc;
	std::vector<uint8_t> payload;

	if (!DecryptPacket(data.data(), data.size(), payloadType, seq, timestamp, ssrc, payload))
		return;

	// Filter by video SSRC
	if (ssrc != m_videoSSRC)
		return;

	std::lock_guard<std::mutex> lock(m_mutex);

	// Check for new frame (timestamp changed)
	if (m_hasTimestamp && timestamp != m_currentTimestamp)
	{
		FlushFrame(m_currentTimestamp);
	}

	m_currentTimestamp = timestamp;
	m_hasTimestamp = true;

	ProcessPayload(payload.data(), payload.size(), seq, timestamp);

	// Check marker bit (indicates end of access unit)
	bool marker = (data[1] & 0x80) != 0;
	if (marker)
	{
		FlushFrame(timestamp);
	}
}

bool VideoRTPReceiver::DecryptPacket(const uint8_t* data, size_t len,
                                      uint8_t& payloadType, uint16_t& seq,
                                      uint32_t& timestamp, uint32_t& ssrc,
                                      std::vector<uint8_t>& payload)
{
	if (len < 12 + crypto_aead_xchacha20poly1305_ietf_ABYTES + 4)
		return false;

	// Parse RTP header
	payloadType = data[1] & 0x7F;
	seq = (data[2] << 8) | data[3];
	timestamp = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
	ssrc = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];

	// Extract 4-byte nonce from end
	uint32_t nonce;
	std::memcpy(&nonce, data + len - 4, sizeof(uint32_t));

	// Build nonce bytes
	std::array<uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonceBytes{};
	std::memcpy(nonceBytes.data(), &nonce, sizeof(uint32_t));

	// Ciphertext is between header and nonce
	const uint8_t* ciphertext = data + 12;
	size_t ciphertextLen = len - 12 - 4;

	// Decrypt
	payload.resize(ciphertextLen - crypto_aead_xchacha20poly1305_ietf_ABYTES);
	unsigned long long decryptedLen;

	int ret = crypto_aead_xchacha20poly1305_ietf_decrypt(
		payload.data(), &decryptedLen,
		nullptr,
		ciphertext, ciphertextLen,
		data, 12,  // AAD = RTP header
		nonceBytes.data(),
		m_secretKey.data());

	if (ret != 0)
		return false;

	payload.resize(static_cast<size_t>(decryptedLen));
	return true;
}

void VideoRTPReceiver::ProcessPayload(const uint8_t* payload, size_t len, uint16_t seq, uint32_t timestamp)
{
	if (len == 0)
		return;

	uint8_t nalType = payload[0] & 0x1F;

	if (nalType >= 1 && nalType <= 23)
	{
		// Single NAL unit — add start code + NAL directly
		m_frameBuffer.push_back(0x00);
		m_frameBuffer.push_back(0x00);
		m_frameBuffer.push_back(0x00);
		m_frameBuffer.push_back(0x01);
		m_frameBuffer.insert(m_frameBuffer.end(), payload, payload + len);
	}
	else if (nalType == 28)
	{
		// FU-A fragmented NAL unit (RFC 6184)
		if (len < 2)
			return;

		uint8_t fuHeader = payload[1];
		bool startBit = (fuHeader & 0x80) != 0;
		bool endBit = (fuHeader & 0x40) != 0;
		uint8_t origNalType = fuHeader & 0x1F;

		if (startBit)
		{
			// Start of fragmented NAL
			m_fuaBuffer.clear();
			m_fuaInProgress = true;

			// Reconstruct original NAL header
			uint8_t origHeader = (payload[0] & 0xE0) | origNalType;
			m_fuaBuffer.push_back(origHeader);
		}

		if (m_fuaInProgress)
		{
			// Append fragment payload (skip FU indicator + FU header)
			m_fuaBuffer.insert(m_fuaBuffer.end(), payload + 2, payload + len);

			if (endBit)
			{
				// Complete — add start code + reassembled NAL
				m_frameBuffer.push_back(0x00);
				m_frameBuffer.push_back(0x00);
				m_frameBuffer.push_back(0x00);
				m_frameBuffer.push_back(0x01);
				m_frameBuffer.insert(m_frameBuffer.end(), m_fuaBuffer.begin(), m_fuaBuffer.end());

				m_fuaBuffer.clear();
				m_fuaInProgress = false;
			}
		}
	}
	else if (nalType == 24)
	{
		// STAP-A: multiple NALs in one packet
		size_t offset = 1;
		while (offset + 2 <= len)
		{
			uint16_t nalSize = (payload[offset] << 8) | payload[offset + 1];
			offset += 2;

			if (offset + nalSize > len)
				break;

			m_frameBuffer.push_back(0x00);
			m_frameBuffer.push_back(0x00);
			m_frameBuffer.push_back(0x00);
			m_frameBuffer.push_back(0x01);
			m_frameBuffer.insert(m_frameBuffer.end(), payload + offset, payload + offset + nalSize);

			offset += nalSize;
		}
	}
}

void VideoRTPReceiver::FlushFrame(uint32_t timestamp)
{
	if (m_frameBuffer.empty())
		return;

	if (m_frameCallback)
		m_frameCallback(m_frameBuffer.data(), m_frameBuffer.size(), timestamp);

	m_frameBuffer.clear();
	m_hasTimestamp = false;
}
