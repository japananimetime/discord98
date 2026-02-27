#include "../include/voice_client.h"
#include "../include/audio_engine.h"
#include "../deps/nlohmann_json.hpp"
#include <sodium.h>
#include <chrono>
#include <cstring>
#include <sstream>
#include <algorithm>

using json = nlohmann::json;

namespace dv {

VoiceClient::VoiceClient() {
    if (sodium_init() == -1) {
        Log(LOG_ERROR, "sodium_init() failed");
    }

    m_udp.SetDataCallback([this](const std::vector<uint8_t> &data) {
        OnUDPData(data);
    });
}

VoiceClient::~VoiceClient() {
    if (IsConnected() || IsConnecting()) {
        Stop();
    }
}

void VoiceClient::SetWebSocket(IVoiceWebSocket *ws) {
    m_ws = ws;
    if (m_ws) {
        m_ws->SetOpenCallback([this]() { OnWebSocketOpen(); });
        m_ws->SetCloseCallback([this](uint16_t code, const std::string &reason) { OnWebSocketClose(code, reason); });
        m_ws->SetMessageCallback([this](const std::string &msg) { OnWebSocketMessage(msg); });
    }
}

void VoiceClient::SetServerInfo(const VoiceServerInfo &info) {
    m_info = info;
}

void VoiceClient::SetAudioEngine(AudioEngine *engine) {
    m_audio = engine;
    if (m_audio) {
        m_audio->SetOpusPacketCallback([this](const uint8_t *data, int size) {
            if (IsConnected()) {
                SendOpusPacket(data, size);
            }
        });
    }
}

void VoiceClient::Start() {
    if (IsConnected() || IsConnecting()) {
        Stop();
    }
    if (!m_ws) {
        Log(LOG_ERROR, "No WebSocket set, cannot start");
        return;
    }
    if (!m_audio) {
        Log(LOG_ERROR, "No AudioEngine set, cannot start");
        return;
    }

    SetState(VoiceState::Connecting);
    {
        std::lock_guard<std::mutex> lk(m_map_mutex);
        m_ssrc_map.clear();
    }
    m_heartbeat_running = true;

    std::string url = "wss://" + m_info.endpoint + "/?v=7";
    Log(LOG_INFO, "Connecting to voice gateway: " + url);
    m_ws->Connect(url);
}

void VoiceClient::Stop() {
    if (!IsConnected() && !IsConnecting()) {
        return;
    }

    SetState(VoiceState::Disconnected);

    if (m_ws) m_ws->Close(4014);
    m_udp.Stop();

    m_heartbeat_running = false;
    if (m_heartbeat_thread.joinable()) m_heartbeat_thread.join();
    if (m_keepalive_thread.joinable()) m_keepalive_thread.join();

    {
        std::lock_guard<std::mutex> lk(m_map_mutex);
        m_ssrc_map.clear();
    }

    if (m_audio) {
        m_audio->StopCapture();
        m_audio->RemoveAllSSRCs();
    }
}

bool VoiceClient::IsConnected() const noexcept {
    return m_state == VoiceState::Connected;
}

bool VoiceClient::IsConnecting() const noexcept {
    return m_state == VoiceState::Connecting || m_state == VoiceState::Establishing;
}

VoiceState VoiceClient::GetState() const noexcept {
    return m_state;
}

void VoiceClient::SetUserVolume(Snowflake user_id, float volume) {
    std::lock_guard<std::mutex> lk(m_map_mutex);
    m_user_volumes[user_id] = volume;
    if (auto it = m_ssrc_map.find(user_id); it != m_ssrc_map.end()) {
        if (m_audio) m_audio->SetVolumeSSRC(it->second, volume);
    }
}

float VoiceClient::GetUserVolume(Snowflake user_id) const {
    std::lock_guard<std::mutex> lk(m_map_mutex);
    if (auto it = m_user_volumes.find(user_id); it != m_user_volumes.end()) {
        return it->second;
    }
    return 1.0f;
}

std::optional<uint32_t> VoiceClient::GetSSRCOfUser(Snowflake user_id) const {
    std::lock_guard<std::mutex> lk(m_map_mutex);
    if (auto it = m_ssrc_map.find(user_id); it != m_ssrc_map.end()) {
        return it->second;
    }
    return std::nullopt;
}

void VoiceClient::SetStateCallback(StateCallback cb) {
    m_state_callback = std::move(cb);
}

void VoiceClient::SetSpeakingCallback(SpeakingCallback cb) {
    m_speaking_callback = std::move(cb);
}

void VoiceClient::SetLogCallback(LogCallback cb) {
    m_log_callback = std::move(cb);
    m_udp.SetLogCallback(cb);
}

void VoiceClient::SendOpusPacket(const uint8_t *data, size_t len) {
    if (!IsConnected() || !m_audio) return;
    uint32_t ts = m_audio->GetRTPTimestamp();
    m_udp.SendEncrypted(data, len, ts);
}

// --- WebSocket event handlers ---

void VoiceClient::OnWebSocketOpen() {
    Log(LOG_INFO, "Voice WebSocket opened");
    SetState(VoiceState::Establishing);
}

void VoiceClient::OnWebSocketClose(uint16_t code, const std::string &reason) {
    Log(LOG_INFO, "Voice WebSocket closed: " + std::to_string(code) + " (" + reason + ")");
    if (m_state != VoiceState::Disconnected) {
        SetState(VoiceState::Disconnected);
    }
}

void VoiceClient::OnWebSocketMessage(const std::string &msg) {
    try {
        auto j = json::parse(msg);
        int op = j.at("op").get<int>();
        std::string data_str = j.at("d").dump();

        switch (static_cast<VoiceGatewayOp>(op)) {
            case VoiceGatewayOp::Hello:
                HandleHello(data_str);
                break;
            case VoiceGatewayOp::Ready:
                HandleReady(data_str);
                break;
            case VoiceGatewayOp::SessionDescription:
                HandleSessionDescription(data_str);
                break;
            case VoiceGatewayOp::Speaking:
                HandleSpeaking(data_str);
                break;
            case VoiceGatewayOp::HeartbeatAck:
                break; // Expected, ignore
            default:
                Log(LOG_DEBUG, "Unhandled voice opcode: " + std::to_string(op));
                break;
        }
    } catch (const std::exception &e) {
        Log(LOG_ERROR, std::string("Failed to parse voice gateway message: ") + e.what());
    }
}

// --- Voice Gateway message handlers ---

void VoiceClient::HandleHello(const std::string &data_json) {
    auto d = json::parse(data_json);
    m_heartbeat_msec = d.at("heartbeat_interval").get<int>();
    Log(LOG_INFO, "Voice Hello, heartbeat interval: " + std::to_string(m_heartbeat_msec) + "ms");

    m_heartbeat_thread = std::thread(&VoiceClient::HeartbeatThread, this);
    SendIdentify();
}

void VoiceClient::HandleReady(const std::string &data_json) {
    auto d = json::parse(data_json);
    m_server_ip = d.at("ip").get<std::string>();
    m_server_port = d.at("port").get<uint16_t>();
    m_ssrc = d.at("ssrc").get<uint32_t>();

    auto modes = d.at("modes").get<std::vector<std::string>>();
    if (std::find(modes.begin(), modes.end(), "aead_xchacha20_poly1305_rtpsize") == modes.end()) {
        Log(LOG_WARN, "aead_xchacha20_poly1305_rtpsize not in supported modes!");
    }

    Log(LOG_INFO, "Voice Ready: " + m_server_ip + ":" + std::to_string(m_server_port) +
        " SSRC=" + std::to_string(m_ssrc));

    m_udp.Connect(m_server_ip, m_server_port);
    m_keepalive_thread = std::thread(&VoiceClient::KeepaliveThread, this);
    DoIPDiscovery();
}

void VoiceClient::HandleSessionDescription(const std::string &data_json) {
    auto d = json::parse(data_json);
    std::string mode = d.at("mode").get<std::string>();
    auto key = d.at("secret_key").get<std::vector<uint8_t>>();

    if (key.size() != 32) {
        Log(LOG_ERROR, "Invalid secret key size: " + std::to_string(key.size()));
        return;
    }

    std::copy(key.begin(), key.end(), m_secret_key.begin());
    m_udp.SetSSRC(m_ssrc);
    m_udp.SetSecretKey(m_secret_key);

    Log(LOG_INFO, "Session established, mode: " + mode);

    // Send initial speaking + silence frame
    SendSpeaking(SpeakingFlags::Microphone);

    // Send silence frame to "open" the connection
    const uint8_t silence[] = {0xF8, 0xFF, 0xFE};
    m_udp.SendEncrypted(silence, sizeof(silence), 0);
    m_udp.Run();

    // Start audio capture
    if (m_audio) {
        m_audio->StartCapture();
    }

    SetState(VoiceState::Connected);
}

void VoiceClient::HandleSpeaking(const std::string &data_json) {
    auto d = json::parse(data_json);
    SpeakingInfo info;
    // Discord sends user_id as a string snowflake
    if (d.at("user_id").is_string())
        info.user_id = std::stoull(d.at("user_id").get<std::string>());
    else
        info.user_id = d.at("user_id").get<uint64_t>();
    info.ssrc = d.at("ssrc").get<uint32_t>();
    info.speaking = static_cast<SpeakingFlags>(d.at("speaking").get<uint32_t>());

    {
        std::lock_guard<std::mutex> lk(m_map_mutex);
        // Apply pre-set volume if user was already configured
        if (auto vol_it = m_user_volumes.find(info.user_id); vol_it != m_user_volumes.end()) {
            if (m_ssrc_map.find(info.user_id) == m_ssrc_map.end()) {
                if (m_audio) m_audio->SetVolumeSSRC(info.ssrc, vol_it->second);
            }
        }
        m_ssrc_map[info.user_id] = info.ssrc;
    }

    if (m_audio) {
        m_audio->AddSSRC(info.ssrc);
    }

    if (m_speaking_callback) {
        m_speaking_callback(info);
    }
}

// --- Outgoing messages ---

void VoiceClient::SendIdentify() {
    json j;
    j["op"] = static_cast<int>(VoiceGatewayOp::Identify);
    j["d"]["server_id"] = std::to_string(m_info.server_id);
    j["d"]["user_id"] = std::to_string(m_info.user_id);
    j["d"]["session_id"] = m_info.session_id;
    j["d"]["token"] = m_info.token;
    j["d"]["video"] = m_info.video;

    SendJson(j.dump());
}

void VoiceClient::DoIPDiscovery() {
    // Build 74-byte IP discovery request
    std::vector<uint8_t> payload(74, 0);
    // Type: request (0x0001)
    payload[0] = 0x00;
    payload[1] = 0x01;
    // Length: 70
    payload[2] = 0x00;
    payload[3] = 0x46;
    // SSRC (big-endian)
    payload[4] = (m_ssrc >> 24) & 0xFF;
    payload[5] = (m_ssrc >> 16) & 0xFF;
    payload[6] = (m_ssrc >> 8) & 0xFF;
    payload[7] = (m_ssrc >> 0) & 0xFF;

    m_udp.Send(payload.data(), payload.size());

    // Wait for discovery response
    constexpr int MAX_TRIES = 100;
    for (int i = 1; i <= MAX_TRIES; i++) {
        auto response = m_udp.Receive();
        if (response.size() >= 74 && response[0] == 0x00 && response[1] == 0x02) {
            const char *ip = reinterpret_cast<const char *>(response.data() + 8);
            uint16_t port = (response[72] << 8) | response[73];
            Log(LOG_INFO, "IP Discovery: " + std::string(ip) + ":" + std::to_string(port));
            SendSelectProtocol(ip, port);
            return;
        } else {
            Log(LOG_WARN, "Non-discovery packet received (try " + std::to_string(i) + "/" + std::to_string(MAX_TRIES) + ")");
        }
    }
    Log(LOG_ERROR, "IP Discovery failed after " + std::to_string(MAX_TRIES) + " tries");
}

void VoiceClient::SendSelectProtocol(const std::string &ip, uint16_t port) {
    json j;
    j["op"] = static_cast<int>(VoiceGatewayOp::SelectProtocol);
    j["d"]["protocol"] = "udp";
    j["d"]["data"]["address"] = ip;
    j["d"]["data"]["port"] = port;
    j["d"]["data"]["mode"] = "aead_xchacha20_poly1305_rtpsize";

    SendJson(j.dump());
}

void VoiceClient::SendSpeaking(SpeakingFlags flags) {
    json j;
    j["op"] = static_cast<int>(VoiceGatewayOp::Speaking);
    j["d"]["speaking"] = static_cast<uint32_t>(flags);
    j["d"]["delay"] = 0;
    j["d"]["ssrc"] = m_ssrc;

    SendJson(j.dump());
}

void VoiceClient::SendJson(const std::string &json_str) {
    if (m_ws) {
        m_ws->Send(json_str);
    }
}

// --- Background threads ---

void VoiceClient::HeartbeatThread() {
    while (m_heartbeat_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_heartbeat_msec));
        if (!m_heartbeat_running) break;

        auto ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());

        json j;
        j["op"] = static_cast<int>(VoiceGatewayOp::Heartbeat);
        j["d"] = ms;
        SendJson(j.dump());
    }
}

void VoiceClient::KeepaliveThread() {
    while (m_heartbeat_running) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (!m_heartbeat_running) break;

        if (IsConnected()) {
            const uint8_t keepalive[] = {0x13, 0x37};
            m_udp.Send(keepalive, sizeof(keepalive));
        }
    }
}

// --- UDP receive handling ---

static size_t GetPayloadOffset(const uint8_t *buf, size_t num_bytes) {
    const bool has_extension = (buf[0] & 0b00010000) != 0;
    const int csrc_count = buf[0] & 0b00001111;
    size_t offset = 12 + csrc_count * 4;
    if (has_extension && num_bytes > offset + 4) {
        offset += 4 + 4 * ((buf[offset + 2] << 8) | buf[offset + 3]);
    }
    return offset;
}

void VoiceClient::OnUDPData(const std::vector<uint8_t> &data) {
    if (data.size() < 16) return; // Too small for RTP + any payload

    uint32_t ssrc = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];

    // Extract 4-byte nonce from end, expand to 24-byte nonce
    std::array<uint8_t, 24> nonce{};
    std::memcpy(nonce.data(), data.data() + data.size() - sizeof(uint32_t), sizeof(uint32_t));

    const bool has_extension = (data[0] & 0b00010000) != 0;
    size_t ext_size = has_extension ? 4 : 0;

    // Decrypt
    unsigned long long mlen = 0;
    size_t aad_len = 12 + ext_size;
    size_t ciphertext_start = aad_len;
    size_t ciphertext_len = data.size() - ciphertext_start - sizeof(uint32_t);

    // We need a mutable buffer for in-place decryption
    std::vector<uint8_t> decrypted(ciphertext_len);

    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            decrypted.data(), &mlen, nullptr,
            data.data() + ciphertext_start, ciphertext_len,
            data.data(), aad_len,
            nonce.data(), m_secret_key.data()) != 0) {
        // Decryption failed, silently ignore
        return;
    }

    if (m_audio && mlen > 0) {
        // Find actual Opus payload offset (skip RTP extensions in decrypted data)
        const auto opus_offset = GetPayloadOffset(data.data(), data.size()) - ciphertext_start;
        if (opus_offset < mlen) {
            m_audio->FeedMeOpus(ssrc, {decrypted.data() + opus_offset, decrypted.data() + mlen});
        } else {
            m_audio->FeedMeOpus(ssrc, {decrypted.data(), decrypted.data() + mlen});
        }
    }
}

// --- State management ---

void VoiceClient::SetState(VoiceState state) {
    m_state = state;
    const char *names[] = {"Disconnected", "Connecting", "Establishing", "Connected"};
    Log(LOG_INFO, std::string("State -> ") + names[static_cast<int>(state)]);
    if (m_state_callback) {
        m_state_callback(state);
    }
}

void VoiceClient::Log(int level, const std::string &msg) {
    if (m_log_callback) m_log_callback(level, "[VoiceClient] " + msg);
}

} // namespace dv
