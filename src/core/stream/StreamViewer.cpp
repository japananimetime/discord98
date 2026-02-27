#include <winsock2.h>
#include <ws2tcpip.h>

#include "StreamViewer.hpp"
#include "../voice/VoiceGateway.hpp"
#include <discord_voice.h>
#include "../DiscordInstance.hpp"
#include "../Frontend.hpp"
#include "../network/WebsocketClient.hpp"
#include "../utils/Util.hpp"
#include "VideoRTPReceiver.hpp"
#include "H264Decoder.hpp"

struct StreamViewer::Impl
{
	dv::VoiceClient viewerVoiceClient;
	VoiceGatewaySocket viewerSocket;
	VideoRTPReceiver rtpReceiver;
	H264Decoder decoder;
};

static void ViewerLog(const char* msg)
{
#ifdef USE_DEBUG_PRINTS
	DbgPrintF("StreamViewer: %s", msg);
#else
	(void)msg;
#endif
}

StreamViewer::StreamViewer()
	: m_impl(std::make_unique<Impl>())
{
}

StreamViewer::~StreamViewer()
{
	Shutdown();
}

void StreamViewer::Init(DiscordInstance* pDiscord)
{
	m_pDiscord = pDiscord;

	m_impl->viewerVoiceClient.SetLogCallback([](int level, const std::string& msg) {
		ViewerLog(("ViewerVoiceClient: [" + std::to_string(level) + "] " + msg).c_str());
	});

	m_impl->viewerVoiceClient.SetStateCallback([this](dv::VoiceState state) {
		ViewerLog(("Viewer voice state changed: " + std::to_string((int)state)).c_str());

		if (state == dv::VoiceState::Connected)
		{
			ViewerLog("Viewer voice connected! Setting up receive pipeline");

			// Get SSRC and secret key
			uint32_t audioSSRC = m_impl->viewerVoiceClient.GetSSRC();
			uint32_t videoSSRC = audioSSRC + 1;
			const auto& secretKey = m_impl->viewerVoiceClient.GetSecretKey();

			// Initialize RTP receiver
			m_impl->rtpReceiver.Init(videoSSRC, secretKey);

			// Initialize H.264 decoder
			if (!m_impl->decoder.Init(1280, 720))
			{
				ViewerLog("H264 decoder init failed");
			}

			// Set up frame callback on RTP receiver
			m_impl->rtpReceiver.SetFrameCallback(
				[this](const uint8_t* h264Data, size_t len, uint32_t timestamp)
				{
					std::vector<uint8_t> pixels;
					int w = 0, h = 0;

					if (m_impl->decoder.Decode(h264Data, len, pixels, w, h))
					{
						if (m_frameCallback && !pixels.empty())
							m_frameCallback(pixels.data(), w, h);
					}
				}
			);

			// Register UDP data callback to feed packets to RTP receiver
			dv::UDPSocket& udp = m_impl->viewerVoiceClient.GetUDPSocket();
			udp.SetDataCallback([this](const std::vector<uint8_t>& data) {
				m_impl->rtpReceiver.Feed(data);
			});

			// Send video opcode to indicate we want to receive video
			{
				nlohmann::json j;
				j["op"] = static_cast<int>(dv::VoiceGatewayOp::Video);

				nlohmann::json d;
				d["audio_ssrc"] = audioSSRC;
				d["video_ssrc"] = 0; // We're receiving, not sending
				d["rtx_ssrc"] = 0;
				d["streams"] = nlohmann::json::array();
				d["codecs"] = nlohmann::json::array();

				j["d"] = d;
				m_impl->viewerSocket.Send(j.dump());
			}

			m_pipelineRunning = true;
		}

		GetFrontend()->OnStreamStateChange();
	});
}

void StreamViewer::Shutdown()
{
	Disconnect();
}

void StreamViewer::WatchStream(const std::string& streamKey)
{
	ViewerLog(("WatchStream: " + streamKey).c_str());

	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_streamKey.empty())
		Disconnect();

	m_streamKey = streamKey;
	m_hasServerInfo = false;

	// Send STREAM_WATCH (opcode 20)
	m_pDiscord->SendStreamWatch(streamKey);

	GetFrontend()->OnStreamStateChange();
}

void StreamViewer::StopWatching()
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_streamKey.empty())
		return;

	std::string key = m_streamKey;
	Disconnect();

	// Send stream delete to stop watching
	m_pDiscord->SendStreamDelete(key);
}

bool StreamViewer::IsWatching() const
{
	return m_impl->viewerVoiceClient.IsConnected() && m_pipelineRunning;
}

bool StreamViewer::IsConnecting() const
{
	return (!m_streamKey.empty() && !IsWatching());
}

void StreamViewer::OnStreamServerUpdate(const std::string& streamKey,
                                         const std::string& endpoint, const std::string& token)
{
	ViewerLog(("OnStreamServerUpdate: endpoint=" + endpoint).c_str());
	std::lock_guard<std::mutex> lock(m_mutex);

	if (streamKey != m_streamKey)
	{
		ViewerLog("OnStreamServerUpdate: stream key mismatch, ignoring");
		return;
	}

	m_pendingEndpoint = endpoint;
	m_pendingToken = token;
	m_hasServerInfo = true;

	TryConnect();
}

void StreamViewer::OnStreamDelete(const std::string& streamKey)
{
	ViewerLog(("OnStreamDelete: key=" + streamKey).c_str());
	std::lock_guard<std::mutex> lock(m_mutex);

	if (streamKey != m_streamKey)
		return;

	Disconnect();
	GetFrontend()->OnStreamStateChange();
}

void StreamViewer::OnWebSocketMessage(int connId, const std::string& payload)
{
	if (m_impl->viewerSocket.GetConnectionID() == connId)
	{
		m_impl->viewerSocket.OnWebSocketMessage(payload);
	}
}

void StreamViewer::OnWebSocketClose(int connId, int errorCode, const std::string& message)
{
	if (m_impl->viewerSocket.GetConnectionID() == connId)
	{
		m_impl->viewerSocket.OnWebSocketClose(errorCode, message);
	}
}

int StreamViewer::GetViewerConnectionID() const
{
	return m_impl->viewerSocket.GetConnectionID();
}

void StreamViewer::TryConnect()
{
	if (!m_hasServerInfo)
		return;

	ViewerLog("TryConnect: connecting to stream voice server");

	m_impl->viewerVoiceClient.Stop();

	dv::VoiceServerInfo info;
	info.endpoint = m_pendingEndpoint;
	info.token = m_pendingToken;
	info.session_id = m_streamKey;
	info.server_id = 0; // Will be extracted from stream key
	info.user_id = (dv::Snowflake)m_pDiscord->GetUserID();
	info.video = true;

	// Parse guild ID from stream key: "guild:GUILDID:CHANNELID:USERID"
	{
		size_t pos1 = m_streamKey.find(':');
		if (pos1 != std::string::npos)
		{
			size_t pos2 = m_streamKey.find(':', pos1 + 1);
			if (pos2 != std::string::npos)
			{
				std::string guildStr = m_streamKey.substr(pos1 + 1, pos2 - pos1 - 1);
				info.server_id = std::stoull(guildStr);
			}
		}
	}

	m_impl->viewerVoiceClient.SetWebSocket(&m_impl->viewerSocket);
	m_impl->viewerVoiceClient.SetServerInfo(info);

	try {
		m_impl->viewerVoiceClient.Start();
		ViewerLog("TryConnect: Start() returned OK");
	}
	catch (const std::exception& e) {
		std::string err = "TryConnect: Start() threw: ";
		err += e.what();
		ViewerLog(err.c_str());
	}
	catch (...) {
		ViewerLog("TryConnect: Start() threw unknown exception");
	}

	m_hasServerInfo = false;
}

void StreamViewer::Disconnect()
{
	m_pipelineRunning = false;

	m_impl->viewerVoiceClient.Stop();
	m_impl->decoder.Shutdown();

	m_streamKey.clear();
	m_hasServerInfo = false;
}
