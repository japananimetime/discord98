#pragma once

#include <string>
#include <memory>
#include <mutex>
#include "../models/Snowflake.hpp"

class DiscordInstance;

class StreamManager
{
public:
	StreamManager();
	~StreamManager();

	void Init(DiscordInstance* pDiscord);
	void Shutdown();

	// Stream source selection
	struct StreamSource
	{
		bool useWindow = false;
		int adapterIndex = 0;
		int outputIndex = 0;
		void* hwnd = nullptr;  // HWND
	};

	// Stream control
	void StartStream(Snowflake guild, Snowflake channel);
	void SetStreamSource(const StreamSource& source) { m_source = source; }
	void StopStream();
	void SetPaused(bool paused);

	// State queries
	bool IsStreaming() const;
	bool IsConnecting() const;
	const std::string& GetStreamKey() const { return m_streamKey; }
	Snowflake GetGuildID() const { return m_guildId; }
	Snowflake GetChannelID() const { return m_channelId; }

	// Called by DiscordInstance dispatch handlers
	void OnStreamCreate(const std::string& streamKey);
	void OnStreamServerUpdate(const std::string& streamKey,
	                          const std::string& endpoint, const std::string& token);
	void OnStreamDelete(const std::string& streamKey);

	// Called by Frontend when a WebSocket message arrives
	void OnWebSocketMessage(int connId, const std::string& payload);
	void OnWebSocketClose(int connId, int errorCode, const std::string& message);

	// Get the stream WebSocket connection ID (for routing messages)
	int GetStreamConnectionID() const;

private:
	void TryConnect();
	void Disconnect();
	void StartPipeline();
	void StopPipeline();

	DiscordInstance* m_pDiscord = nullptr;

	// PIMPL: hide voice/video library types from the header
	struct Impl;
	std::unique_ptr<Impl> m_impl;

	// Target channel
	Snowflake m_guildId = 0;
	Snowflake m_channelId = 0;

	// Stream key from Discord
	std::string m_streamKey;

	// Pending connection info (two-piece connect)
	std::string m_pendingEndpoint;
	std::string m_pendingToken;
	bool m_hasStreamKey = false;
	bool m_hasServerInfo = false;

	// Pipeline running
	bool m_pipelineRunning = false;

	// Stream source
	StreamSource m_source;

	std::mutex m_mutex;
};
