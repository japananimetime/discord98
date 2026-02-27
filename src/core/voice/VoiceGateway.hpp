#pragma once

#include <discord_voice.h>
#include "../network/WebsocketClient.hpp"

// Implements dv::IVoiceWebSocket using the existing WebsocketClient (websocketpp).
// This creates a second WebSocket connection separate from the main gateway.
class VoiceGatewaySocket : public dv::IVoiceWebSocket
{
public:
	VoiceGatewaySocket()
	{
	}

	~VoiceGatewaySocket() override
	{
		Close();
	}

	void Connect(const std::string& url) override
	{
		// Clean up any previous connection
		Close();

		// Reset state for new connection
		m_closed = false;

		// VoiceClient::Start() already formats the URL as wss://endpoint/?v=7
		// so just pass it through directly
		m_connId = GetWebsocketClient()->Connect(url);

		if (m_connId < 0)
		{
			NotifyClose(4000, "Failed to connect to voice gateway");
			return;
		}

		// Poll for connection open in a background thread
		m_pollThread = std::thread([this]() {
			// Wait for the connection to open
			for (int i = 0; i < 100; i++) // 10 seconds max
			{
				if (m_closed)
					return;

				auto meta = GetWebsocketClient()->GetMetadata(m_connId);
				if (!meta)
				{
					NotifyClose(4000, "Connection metadata lost");
					return;
				}

				if (meta->GetStatus() == WSConnectionMetadata::OPEN)
				{
					NotifyOpen();
					return;
				}

				if (meta->GetStatus() == WSConnectionMetadata::FAILED)
				{
					NotifyClose(4000, "Voice WebSocket connection failed");
					return;
				}

				if (meta->GetStatus() == WSConnectionMetadata::CLOSED)
				{
					NotifyClose(4000, "Voice WebSocket closed during connect");
					return;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}

			NotifyClose(4009, "Voice WebSocket connection timed out");
		});
	}

	void Send(const std::string& json_str) override
	{
		if (m_connId >= 0)
			GetWebsocketClient()->SendMsg(m_connId, json_str);
	}

	void Close(uint16_t code = 1000) override
	{
		m_closed = true;

		if (m_pollThread.joinable())
			m_pollThread.join();

		if (m_connId >= 0)
		{
			GetWebsocketClient()->Close(m_connId, (websocketpp::close::status::value)code);
			m_connId = -1;
		}
	}

	// Called by the frontend when a WebSocket message arrives for this connection
	void OnWebSocketMessage(const std::string& payload)
	{
		NotifyMessage(payload);
	}

	// Called by the frontend when this WebSocket connection closes
	void OnWebSocketClose(int errorCode, const std::string& message)
	{
		NotifyClose((uint16_t)errorCode, message);
	}

	int GetConnectionID() const { return m_connId; }

private:
	int m_connId = -1;
	std::thread m_pollThread;
	std::atomic<bool> m_closed{ false };
};
