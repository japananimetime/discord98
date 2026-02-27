#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <functional>
#include <vector>
#include "../models/Snowflake.hpp"

class DiscordInstance;

class StreamViewer
{
public:
	StreamViewer();
	~StreamViewer();

	void Init(DiscordInstance* pDiscord);
	void Shutdown();

	// Watch a user's stream
	void WatchStream(const std::string& streamKey);
	void StopWatching();

	// State queries
	bool IsWatching() const;
	bool IsConnecting() const;
	const std::string& GetStreamKey() const { return m_streamKey; }

	// Called by DiscordInstance dispatch handlers when watching
	void OnStreamServerUpdate(const std::string& streamKey,
	                           const std::string& endpoint, const std::string& token);
	void OnStreamDelete(const std::string& streamKey);

	// Called by Frontend when a WebSocket message arrives
	void OnWebSocketMessage(int connId, const std::string& payload);
	void OnWebSocketClose(int connId, int errorCode, const std::string& message);

	int GetViewerConnectionID() const;

	// Callback for decoded video frames (BGRA pixel data)
	using FrameCallback = std::function<void(const uint8_t* bgraPixels, int width, int height)>;
	void SetFrameCallback(FrameCallback cb) { m_frameCallback = std::move(cb); }

private:
	void TryConnect();
	void Disconnect();

	DiscordInstance* m_pDiscord = nullptr;

	struct Impl;
	std::unique_ptr<Impl> m_impl;

	std::string m_streamKey;

	std::string m_pendingEndpoint;
	std::string m_pendingToken;
	bool m_hasServerInfo = false;
	bool m_pipelineRunning = false;

	FrameCallback m_frameCallback;

	std::mutex m_mutex;
};
