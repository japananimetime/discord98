#include <winsock2.h>
#include <ws2tcpip.h>

#include "VoiceManager.hpp"
#include "VoiceGateway.hpp"
#include <discord_voice.h>
#include "../DiscordInstance.hpp"
#include "../Frontend.hpp"
#include "../network/WebsocketClient.hpp"
#include "../utils/Util.hpp"
#include "../config/LocalSettings.hpp"

struct VoiceManager::Impl
{
	dv::AudioEngine audioEngine;
	dv::VoiceClient voiceClient;
	VoiceGatewaySocket voiceSocket;
};

static void VoiceLog(const char* msg)
{
#ifdef USE_DEBUG_PRINTS
	DbgPrintF("Voice: %s", msg);
#else
	(void)msg;
#endif
}

VoiceManager::VoiceManager()
	: m_impl(std::make_unique<Impl>())
{
}

VoiceManager::~VoiceManager()
{
	Shutdown();
}

void VoiceManager::Init(DiscordInstance* pDiscord)
{
	m_pDiscord = pDiscord;

	// Initialize audio engine
	m_impl->audioEngine.SetLogCallback([](int level, const std::string& msg) {
		VoiceLog(("AudioEngine: [" + std::to_string(level) + "] " + msg).c_str());
	});

	if (m_impl->audioEngine.Init())
	{
		m_audioInitialized = true;
		VoiceLog("Voice audio engine initialized");
	}
	else
	{
		VoiceLog("WARNING: Voice audio engine failed to initialize");
	}

	// Set up voice client callbacks
	m_impl->voiceClient.SetLogCallback([](int level, const std::string& msg) {
		VoiceLog(("VoiceClient: [" + std::to_string(level) + "] " + msg).c_str());
	});

	m_impl->voiceClient.SetStateCallback([this](dv::VoiceState state) {
		VoiceLog(("Voice state changed: " + std::to_string((int)state)).c_str());
		GetFrontend()->OnVoiceStateChange();

		if (state == dv::VoiceState::Connected && m_audioInitialized)
		{
			VoiceLog("Voice connected! Starting audio capture and playback");
			m_impl->audioEngine.StartCapture();
			m_impl->audioEngine.StartPlayback();

			if (m_selfMute)
				m_impl->audioEngine.SetCaptureEnabled(false);
			if (m_selfDeaf)
				m_impl->audioEngine.SetPlaybackEnabled(false);

			// Apply saved audio settings
			LocalSettings* ls = GetLocalSettings();
			m_impl->audioEngine.SetCaptureGain(ls->GetAudioInputVolume() / 100.0);
			m_impl->audioEngine.SetPlaybackGain(ls->GetAudioOutputVolume() / 100.0);
			m_impl->audioEngine.SetCaptureGate(ls->GetAudioVoiceGate() / 100.0);
			m_impl->audioEngine.SetNoiseSuppress(ls->GetAudioNoiseSuppression());

			// Apply saved input device
			std::string savedInput = ls->GetAudioInputDevice();
			if (!savedInput.empty()) {
				auto& capDevs = m_impl->audioEngine.GetDevices().GetCaptureDevices();
				for (size_t i = 0; i < capDevs.size(); i++) {
					if (capDevs[i].name == savedInput) {
						m_impl->audioEngine.SetCaptureDevice((uint32_t)i);
						break;
					}
				}
			}

			// Apply saved output device
			std::string savedOutput = ls->GetAudioOutputDevice();
			if (!savedOutput.empty()) {
				auto& pbDevs = m_impl->audioEngine.GetDevices().GetPlaybackDevices();
				for (size_t i = 0; i < pbDevs.size(); i++) {
					if (pbDevs[i].name == savedOutput) {
						m_impl->audioEngine.SetPlaybackDevice((uint32_t)i);
						break;
					}
				}
			}
		}
	});

	m_impl->voiceClient.SetSpeakingCallback([](const dv::SpeakingInfo& info) {
		VoiceLog(("User speaking: user=" + std::to_string(info.user_id) + " speaking=" + std::to_string((uint32_t)info.speaking)).c_str());
	});
}

void VoiceManager::Shutdown()
{
	Disconnect();

	if (m_audioInitialized)
	{
		m_impl->audioEngine.Shutdown();
		m_audioInitialized = false;
	}
}

void VoiceManager::JoinVoiceChannel(Snowflake guild, Snowflake channel)
{
	VoiceLog("JoinVoiceChannel: enter");

	std::lock_guard<std::mutex> lock(m_mutex);

	// If already in this channel, do nothing
	if (m_channelId == channel && IsConnected())
		return;

	// If in a different channel, disconnect first
	if (m_channelId != 0)
		Disconnect();

	m_guildId = guild;
	m_channelId = channel;
	m_hasSessionId = false;
	m_hasServerInfo = false;

	VoiceLog("JoinVoiceChannel: sending opcode 4");

	// Send opcode 4 (VOICE_STATE_UPDATE) to the main gateway
	m_pDiscord->SendVoiceStateUpdate(guild, channel, m_selfMute, m_selfDeaf);

	VoiceLog("JoinVoiceChannel: opcode 4 sent, updating UI");

	// Update UI immediately to show "waiting" state
	GetFrontend()->OnVoiceStateChange();

	VoiceLog("JoinVoiceChannel: done");
}

void VoiceManager::LeaveVoiceChannel()
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_channelId == 0)
		return;

	Snowflake guild = m_guildId;

	Disconnect();

	// Send opcode 4 with null channel to leave
	m_pDiscord->SendVoiceStateUpdate(guild, 0, false, false);
}

void VoiceManager::ToggleMute()
{
	m_selfMute = !m_selfMute;

	if (m_audioInitialized)
		m_impl->audioEngine.SetCaptureEnabled(!m_selfMute);

	// Update gateway with new mute state
	if (m_channelId != 0)
		m_pDiscord->SendVoiceStateUpdate(m_guildId, m_channelId, m_selfMute, m_selfDeaf);

	GetFrontend()->OnVoiceStateChange();
}

void VoiceManager::ToggleDeafen()
{
	m_selfDeaf = !m_selfDeaf;

	if (m_audioInitialized)
		m_impl->audioEngine.SetPlaybackEnabled(!m_selfDeaf);

	// Update gateway with new deaf state
	if (m_channelId != 0)
		m_pDiscord->SendVoiceStateUpdate(m_guildId, m_channelId, m_selfMute, m_selfDeaf);

	GetFrontend()->OnVoiceStateChange();
}

bool VoiceManager::IsConnected() const
{
	return m_impl->voiceClient.IsConnected();
}

bool VoiceManager::IsConnecting() const
{
	return m_impl->voiceClient.IsConnecting();
}

std::string VoiceManager::GetChannelName() const
{
	if (m_channelId == 0 || !m_pDiscord)
		return "";

	Channel* pChan = m_pDiscord->GetChannel(m_channelId);
	if (pChan)
		return pChan->m_name;

	return "";
}

void VoiceManager::OnVoiceStateUpdate(const std::string& sessionId, Snowflake userId, Snowflake channelId)
{
	VoiceLog(("OnVoiceStateUpdate: user=" + std::to_string(userId) + " chan=" + std::to_string(channelId)).c_str());
	std::lock_guard<std::mutex> lock(m_mutex);

	// Only care about our own voice state for connection purposes
	if (userId != m_pDiscord->GetUserID())
	{
		VoiceLog("OnVoiceStateUpdate: not our user, ignoring");
		return;
	}

	// If channelId is 0, we were disconnected from the voice channel
	if (channelId == 0)
	{
		VoiceLog("OnVoiceStateUpdate: channelId=0, disconnecting");
		Disconnect();
		GetFrontend()->OnVoiceStateChange();
		return;
	}

	m_pendingSessionId = sessionId;
	m_hasSessionId = true;

	VoiceLog(("OnVoiceStateUpdate: session=" + sessionId + " hasServer=" + std::to_string(m_hasServerInfo)).c_str());

	TryConnect();
	VoiceLog("OnVoiceStateUpdate: done");
}

void VoiceManager::OnVoiceServerUpdate(const std::string& endpoint, const std::string& token, Snowflake guildId)
{
	VoiceLog(("OnVoiceServerUpdate: endpoint=" + endpoint).c_str());
	std::lock_guard<std::mutex> lock(m_mutex);

	m_pendingEndpoint = endpoint;
	m_pendingToken = token;
	m_hasServerInfo = true;

	VoiceLog(("OnVoiceServerUpdate: hasSession=" + std::to_string(m_hasSessionId)).c_str());

	TryConnect();
	VoiceLog("OnVoiceServerUpdate: done");
}

void VoiceManager::OnWebSocketMessage(int connId, const std::string& payload)
{
	if (m_impl->voiceSocket.GetConnectionID() == connId)
	{
		VoiceLog(("OnWebSocketMessage: routing voice msg, len=" + std::to_string(payload.size())).c_str());
		m_impl->voiceSocket.OnWebSocketMessage(payload);
	}
}

void VoiceManager::OnWebSocketClose(int connId, int errorCode, const std::string& message)
{
	if (m_impl->voiceSocket.GetConnectionID() == connId)
	{
		VoiceLog(("OnWebSocketClose: code=" + std::to_string(errorCode) + " msg=" + message).c_str());
		m_impl->voiceSocket.OnWebSocketClose(errorCode, message);
	}
}

int VoiceManager::GetVoiceConnectionID() const
{
	return m_impl->voiceSocket.GetConnectionID();
}

void* VoiceManager::GetAudioEngine()
{
	if (!m_audioInitialized)
		return nullptr;
	return &m_impl->audioEngine;
}

void VoiceManager::TryConnect()
{
	// Need both pieces of info before we can connect
	if (!m_hasSessionId || !m_hasServerInfo)
		return;

	VoiceLog("TryConnect: have both pieces");

	// Stop any existing connection
	m_impl->voiceClient.Stop();

	// Set up voice server info
	dv::VoiceServerInfo info;
	info.endpoint = m_pendingEndpoint;
	info.token = m_pendingToken;
	info.session_id = m_pendingSessionId;
	info.server_id = (dv::Snowflake)m_guildId;
	info.user_id = (dv::Snowflake)m_pDiscord->GetUserID();

	// Configure voice client
	m_impl->voiceClient.SetWebSocket(&m_impl->voiceSocket);
	m_impl->voiceClient.SetServerInfo(info);

	if (m_audioInitialized)
		m_impl->voiceClient.SetAudioEngine(&m_impl->audioEngine);

	std::string logmsg = "TryConnect: Start() endpoint=" + info.endpoint + " user=" + std::to_string(info.user_id);
	VoiceLog(logmsg.c_str());

	try {
		// Start voice connection
		m_impl->voiceClient.Start();
		VoiceLog("TryConnect: Start() returned OK");
	}
	catch (const std::exception& e) {
		std::string err = "TryConnect: Start() threw: ";
		err += e.what();
		VoiceLog(err.c_str());
	}
	catch (...) {
		VoiceLog("TryConnect: Start() threw unknown exception");
	}

	// Clear pending state
	m_hasSessionId = false;
	m_hasServerInfo = false;
}

void VoiceManager::Disconnect()
{
	m_impl->voiceClient.Stop();

	if (m_audioInitialized)
	{
		m_impl->audioEngine.StopCapture();
		m_impl->audioEngine.StopPlayback();
		m_impl->audioEngine.RemoveAllSSRCs();
	}

	m_channelId = 0;
	m_guildId = 0;
	m_hasSessionId = false;
	m_hasServerInfo = false;
}
