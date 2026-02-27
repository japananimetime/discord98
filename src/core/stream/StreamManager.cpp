#include <winsock2.h>
#include <ws2tcpip.h>

#include "StreamManager.hpp"
#include "../voice/VoiceGateway.hpp"
#include <discord_voice.h>
#include "../DiscordInstance.hpp"
#include "../Frontend.hpp"
#include "../network/WebsocketClient.hpp"
#include "../utils/Util.hpp"
#include "ScreenCapture.hpp"
#include "H264Encoder.hpp"
#include "VideoRTPSender.hpp"
#include "LoopbackCapture.hpp"

struct StreamManager::Impl
{
	dv::VoiceClient streamVoiceClient;
	VoiceGatewaySocket streamSocket;
	ScreenCapture screenCapture;
	H264Encoder encoder;
	VideoRTPSender rtpSender;
	LoopbackCapture loopbackCapture;
};

static void StreamLog(const char* msg)
{
#ifdef USE_DEBUG_PRINTS
	DbgPrintF("Stream: %s", msg);
#else
	(void)msg;
#endif
}

StreamManager::StreamManager()
	: m_impl(std::make_unique<Impl>())
{
}

StreamManager::~StreamManager()
{
	Shutdown();
}

void StreamManager::Init(DiscordInstance* pDiscord)
{
	m_pDiscord = pDiscord;

	// Set up stream voice client callbacks
	m_impl->streamVoiceClient.SetLogCallback([](int level, const std::string& msg) {
		StreamLog(("StreamVoiceClient: [" + std::to_string(level) + "] " + msg).c_str());
	});

	m_impl->streamVoiceClient.SetStateCallback([this](dv::VoiceState state) {
		StreamLog(("Stream voice state changed: " + std::to_string((int)state)).c_str());

		if (state == dv::VoiceState::Connected)
		{
			StreamLog("Stream voice connected! Starting video pipeline");
			StartPipeline();
		}

		GetFrontend()->OnStreamStateChange();
	});
}

void StreamManager::Shutdown()
{
	Disconnect();
}

void StreamManager::StartStream(Snowflake guild, Snowflake channel)
{
	StreamLog("StartStream: enter");

	std::lock_guard<std::mutex> lock(m_mutex);

	// If already streaming, stop first
	if (!m_streamKey.empty())
		Disconnect();

	m_guildId = guild;
	m_channelId = channel;
	m_hasStreamKey = false;
	m_hasServerInfo = false;

	StreamLog("StartStream: sending opcode 18 (STREAM_CREATE)");

	m_pDiscord->SendStreamCreate(guild, channel);

	GetFrontend()->OnStreamStateChange();

	StreamLog("StartStream: done");
}

void StreamManager::StopStream()
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_streamKey.empty())
		return;

	std::string key = m_streamKey;

	Disconnect();

	m_pDiscord->SendStreamDelete(key);
}

void StreamManager::SetPaused(bool paused)
{
	if (m_streamKey.empty())
		return;

	m_pDiscord->SendStreamSetPaused(m_streamKey, paused);
}

bool StreamManager::IsStreaming() const
{
	return m_impl->streamVoiceClient.IsConnected() && m_pipelineRunning;
}

bool StreamManager::IsConnecting() const
{
	return (!m_streamKey.empty() && !IsStreaming());
}

void StreamManager::OnStreamCreate(const std::string& streamKey)
{
	StreamLog(("OnStreamCreate: key=" + streamKey).c_str());
	std::lock_guard<std::mutex> lock(m_mutex);

	m_streamKey = streamKey;
	m_hasStreamKey = true;

	TryConnect();
}

void StreamManager::OnStreamServerUpdate(const std::string& streamKey,
                                          const std::string& endpoint, const std::string& token)
{
	StreamLog(("OnStreamServerUpdate: endpoint=" + endpoint).c_str());
	std::lock_guard<std::mutex> lock(m_mutex);

	if (streamKey != m_streamKey)
	{
		StreamLog("OnStreamServerUpdate: stream key mismatch, ignoring");
		return;
	}

	m_pendingEndpoint = endpoint;
	m_pendingToken = token;
	m_hasServerInfo = true;

	TryConnect();
}

void StreamManager::OnStreamDelete(const std::string& streamKey)
{
	StreamLog(("OnStreamDelete: key=" + streamKey).c_str());
	std::lock_guard<std::mutex> lock(m_mutex);

	if (streamKey != m_streamKey)
		return;

	Disconnect();
	GetFrontend()->OnStreamStateChange();
}

void StreamManager::OnWebSocketMessage(int connId, const std::string& payload)
{
	if (m_impl->streamSocket.GetConnectionID() == connId)
	{
		StreamLog(("OnWebSocketMessage: routing stream msg, len=" + std::to_string(payload.size())).c_str());
		m_impl->streamSocket.OnWebSocketMessage(payload);
	}
}

void StreamManager::OnWebSocketClose(int connId, int errorCode, const std::string& message)
{
	if (m_impl->streamSocket.GetConnectionID() == connId)
	{
		StreamLog(("OnWebSocketClose: code=" + std::to_string(errorCode) + " msg=" + message).c_str());
		m_impl->streamSocket.OnWebSocketClose(errorCode, message);
	}
}

int StreamManager::GetStreamConnectionID() const
{
	return m_impl->streamSocket.GetConnectionID();
}

void StreamManager::TryConnect()
{
	// Need both pieces of info before we can connect
	if (!m_hasStreamKey || !m_hasServerInfo)
		return;

	StreamLog("TryConnect: have both pieces");

	// Stop any existing connection
	m_impl->streamVoiceClient.Stop();

	// Set up voice server info for the stream connection
	dv::VoiceServerInfo info;
	info.endpoint = m_pendingEndpoint;
	info.token = m_pendingToken;
	info.session_id = m_streamKey; // Stream uses stream_key as session_id
	info.server_id = (dv::Snowflake)m_guildId;
	info.user_id = (dv::Snowflake)m_pDiscord->GetUserID();
	info.video = true; // This is a video/stream connection

	// Configure stream voice client
	m_impl->streamVoiceClient.SetWebSocket(&m_impl->streamSocket);
	m_impl->streamVoiceClient.SetServerInfo(info);
	// No audio engine for stream connection — we only send video

	std::string logmsg = "TryConnect: Start() endpoint=" + info.endpoint + " user=" + std::to_string(info.user_id);
	StreamLog(logmsg.c_str());

	try {
		m_impl->streamVoiceClient.Start();
		StreamLog("TryConnect: Start() returned OK");
	}
	catch (const std::exception& e) {
		std::string err = "TryConnect: Start() threw: ";
		err += e.what();
		StreamLog(err.c_str());
	}
	catch (...) {
		StreamLog("TryConnect: Start() threw unknown exception");
	}

	// Clear pending state
	m_hasStreamKey = false;
	m_hasServerInfo = false;
}

void StreamManager::Disconnect()
{
	StopPipeline();

	m_impl->streamVoiceClient.Stop();

	m_streamKey.clear();
	m_guildId = 0;
	m_channelId = 0;
	m_hasStreamKey = false;
	m_hasServerInfo = false;
}

void StreamManager::StartPipeline()
{
	if (m_pipelineRunning)
		return;

	StreamLog("StartPipeline: initializing video pipeline");

	// Get SSRC and secret key from the stream voice client
	uint32_t audioSSRC = m_impl->streamVoiceClient.GetSSRC();
	uint32_t videoSSRC = audioSSRC + 1;
	const auto& secretKey = m_impl->streamVoiceClient.GetSecretKey();
	dv::UDPSocket& udp = m_impl->streamVoiceClient.GetUDPSocket();

	// Initialize RTP sender
	m_impl->rtpSender.Init(&udp, videoSSRC, secretKey);

	// Send video codec info (voice gateway opcode 12)
	{
		nlohmann::json j;
		j["op"] = static_cast<int>(dv::VoiceGatewayOp::Video);

		nlohmann::json d;
		d["audio_ssrc"] = audioSSRC;
		d["video_ssrc"] = videoSSRC;
		d["rtx_ssrc"] = audioSSRC + 2;

		nlohmann::json stream;
		stream["type"] = "video";
		stream["rid"] = "100";
		stream["ssrc"] = videoSSRC;
		stream["active"] = true;
		stream["quality"] = 100;
		stream["max_bitrate"] = 2500000;
		stream["max_framerate"] = 30;

		nlohmann::json resolution;
		resolution["type"] = "fixed";
		resolution["width"] = 1280;
		resolution["height"] = 720;
		stream["max_resolution"] = resolution;

		d["streams"] = nlohmann::json::array({ stream });

		nlohmann::json codec;
		codec["name"] = "H264";
		codec["type"] = "video";
		codec["priority"] = 1000;
		codec["payload_type"] = 101;
		codec["rtx_payload_type"] = 102;

		d["codecs"] = nlohmann::json::array({ codec });

		j["d"] = d;

		m_impl->streamSocket.Send(j.dump());
	}

	// Send Speaking with Soundshare flag (bit 1)
	m_impl->streamVoiceClient.SendSpeaking(dv::SpeakingFlags::Soundshare);

	// Initialize screen capture with selected source
	bool captureOk = false;
	if (m_source.useWindow && m_source.hwnd)
	{
		captureOk = m_impl->screenCapture.InitFromWindow((HWND)m_source.hwnd);
		if (!captureOk)
			StreamLog("StartPipeline: window capture init failed, falling back to desktop");
	}
	if (!captureOk)
	{
		captureOk = m_impl->screenCapture.Init(m_source.adapterIndex, m_source.outputIndex);
	}
	if (!captureOk)
	{
		StreamLog("StartPipeline: screen capture init failed");
		return;
	}

	// Initialize H.264 encoder using the screen capture's D3D11 device
	H264Encoder::Config encConfig;
	encConfig.width = m_impl->screenCapture.GetWidth();
	encConfig.height = m_impl->screenCapture.GetHeight();
	encConfig.fps = 30;
	encConfig.bitrate = 2500000;
	encConfig.keyframeInterval = 60;

	if (!m_impl->encoder.Init(encConfig, m_impl->screenCapture.GetDevice()))
	{
		StreamLog("StartPipeline: H264 encoder init failed");
		m_impl->screenCapture.Shutdown();
		return;
	}

	// Set up capture callback: capture frame -> encode -> RTP send
	m_impl->screenCapture.SetFrameCallback(
		[this](ID3D11Texture2D* texture, int width, int height, uint32_t timestamp90kHz)
		{
			std::vector<uint8_t> nalData;
			if (m_impl->encoder.Encode(texture, nalData) && !nalData.empty())
			{
				m_impl->rtpSender.SendFrame(nalData.data(), nalData.size(), timestamp90kHz);
			}
		}
	);

	// Start capturing at 30fps
	m_impl->screenCapture.Start(30);

	// Initialize and start loopback audio capture (system audio)
	if (m_impl->loopbackCapture.Init(&udp, audioSSRC, secretKey))
	{
		m_impl->loopbackCapture.Start();
		StreamLog("StartPipeline: loopback audio capture started");
	}
	else
	{
		StreamLog("StartPipeline: loopback audio capture init failed (non-fatal)");
	}

	m_pipelineRunning = true;
	StreamLog("StartPipeline: video pipeline started");
}

void StreamManager::StopPipeline()
{
	if (!m_pipelineRunning)
		return;

	StreamLog("StopPipeline: shutting down video pipeline");

	m_impl->loopbackCapture.Stop();
	m_impl->loopbackCapture.Shutdown();
	m_impl->screenCapture.Stop();
	m_impl->screenCapture.Shutdown();
	m_impl->encoder.Shutdown();

	m_pipelineRunning = false;
}
