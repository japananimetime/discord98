#pragma once

#include <string>
#include <memory>
#include <mutex>
#include "../models/Snowflake.hpp"

class DiscordInstance;

class VoiceManager
{
public:
	VoiceManager();
	~VoiceManager();

	void Init(DiscordInstance* pDiscord);
	void Shutdown();

	// Voice channel control
	void JoinVoiceChannel(Snowflake guild, Snowflake channel);
	void LeaveVoiceChannel();

	// Audio control
	void ToggleMute();
	void ToggleDeafen();

	// Access underlying audio engine (may return nullptr if not initialized)
	void* GetAudioEngine();
	bool IsMuted() const { return m_selfMute; }
	bool IsDeafened() const { return m_selfDeaf; }

	// State queries
	bool IsConnected() const;
	bool IsConnecting() const;
	bool IsWaitingForServer() const { return m_channelId != 0 && !IsConnected() && !IsConnecting(); }
	Snowflake GetGuildID() const { return m_guildId; }
	Snowflake GetChannelID() const { return m_channelId; }
	std::string GetChannelName() const;

	// Called by DiscordInstance when gateway dispatch events arrive
	void OnVoiceStateUpdate(const std::string& sessionId, Snowflake userId, Snowflake channelId);
	void OnVoiceServerUpdate(const std::string& endpoint, const std::string& token, Snowflake guildId);

	// Called by Frontend when a WebSocket message arrives
	void OnWebSocketMessage(int connId, const std::string& payload);
	void OnWebSocketClose(int connId, int errorCode, const std::string& message);

	// Get the voice WebSocket connection ID (for routing messages)
	int GetVoiceConnectionID() const;

private:
	void TryConnect();
	void Disconnect();

	DiscordInstance* m_pDiscord = nullptr;

	// PIMPL: hide voice library types from the header to avoid winsock2 include order issues
	struct Impl;
	std::unique_ptr<Impl> m_impl;

	// Target channel
	Snowflake m_guildId = 0;
	Snowflake m_channelId = 0;

	// Pending connection info (collected from two gateway events)
	std::string m_pendingSessionId;
	std::string m_pendingEndpoint;
	std::string m_pendingToken;
	bool m_hasSessionId = false;
	bool m_hasServerInfo = false;

	// Self state
	bool m_selfMute = false;
	bool m_selfDeaf = false;

	// Audio engine initialized
	bool m_audioInitialized = false;

	std::mutex m_mutex;
};
