#pragma once
#ifndef DISCORD_VOICE_CLIENT_H
#define DISCORD_VOICE_CLIENT_H

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include "voice_types.h"
#include "udp_socket.h"

namespace dv {

// Abstract WebSocket interface - the host application implements this
// using whatever WebSocket library it already has (websocketpp, IXWebSocket, etc.)
class IVoiceWebSocket {
public:
    virtual ~IVoiceWebSocket() = default;

    virtual void Connect(const std::string &url) = 0;
    virtual void Send(const std::string &json_str) = 0;
    virtual void Close(uint16_t code = 1000) = 0;

    // The host app must call these when events occur:
    using OpenCallback = std::function<void()>;
    using CloseCallback = std::function<void(uint16_t code, const std::string &reason)>;
    using MessageCallback = std::function<void(const std::string &message)>;

    void SetOpenCallback(OpenCallback cb) { m_on_open = std::move(cb); }
    void SetCloseCallback(CloseCallback cb) { m_on_close = std::move(cb); }
    void SetMessageCallback(MessageCallback cb) { m_on_message = std::move(cb); }

protected:
    // Call these from your WebSocket implementation
    void NotifyOpen() { if (m_on_open) m_on_open(); }
    void NotifyClose(uint16_t code, const std::string &reason) { if (m_on_close) m_on_close(code, reason); }
    void NotifyMessage(const std::string &msg) { if (m_on_message) m_on_message(msg); }

private:
    OpenCallback m_on_open;
    CloseCallback m_on_close;
    MessageCallback m_on_message;
};

// Forward declaration
class AudioEngine;

class VoiceClient {
public:
    VoiceClient();
    ~VoiceClient();

    // Set the WebSocket implementation (must be set before Start)
    void SetWebSocket(IVoiceWebSocket *ws);

    // Set connection info (must be set before Start)
    void SetServerInfo(const VoiceServerInfo &info);

    // Set the audio engine (must be set before Start)
    void SetAudioEngine(AudioEngine *engine);

    // Connect / disconnect
    void Start();
    void Stop();

    bool IsConnected() const noexcept;
    bool IsConnecting() const noexcept;
    VoiceState GetState() const noexcept;

    // Per-user volume control
    void SetUserVolume(Snowflake user_id, float volume);
    float GetUserVolume(Snowflake user_id) const;
    std::optional<uint32_t> GetSSRCOfUser(Snowflake user_id) const;

    // Event callbacks
    void SetStateCallback(StateCallback cb);
    void SetSpeakingCallback(SpeakingCallback cb);
    void SetLogCallback(LogCallback cb);

    // Called by AudioEngine when an Opus packet is ready to send
    void SendOpusPacket(const uint8_t *data, size_t len);

    // Send speaking flags (made public for stream manager use)
    void SendSpeaking(SpeakingFlags flags);

    // Getters for stream/video use
    uint32_t GetSSRC() const noexcept { return m_ssrc; }
    const std::array<uint8_t, 32>& GetSecretKey() const noexcept { return m_secret_key; }
    UDPSocket& GetUDPSocket() { return m_udp; }

private:
    void OnWebSocketOpen();
    void OnWebSocketClose(uint16_t code, const std::string &reason);
    void OnWebSocketMessage(const std::string &msg);

    void HandleHello(const std::string &data_json);
    void HandleReady(const std::string &data_json);
    void HandleSessionDescription(const std::string &data_json);
    void HandleSpeaking(const std::string &data_json);

    void SendIdentify();
    void DoIPDiscovery();
    void SendSelectProtocol(const std::string &ip, uint16_t port);
    void SendJson(const std::string &json_str);

    void HeartbeatThread();
    void KeepaliveThread();

    void OnUDPData(const std::vector<uint8_t> &data);

    void SetState(VoiceState state);
    void Log(int level, const std::string &msg);

    // Connection info
    VoiceServerInfo m_info;

    // WebSocket
    IVoiceWebSocket *m_ws = nullptr;

    // UDP
    UDPSocket m_udp;

    // Audio engine
    AudioEngine *m_audio = nullptr;

    // State
    std::atomic<VoiceState> m_state{VoiceState::Disconnected};

    // SSRC mapping
    std::unordered_map<Snowflake, uint32_t> m_ssrc_map;
    std::unordered_map<Snowflake, float> m_user_volumes;
    mutable std::mutex m_map_mutex;

    // Session data
    std::array<uint8_t, 32> m_secret_key{};
    uint32_t m_ssrc = 0;
    std::string m_server_ip;
    uint16_t m_server_port = 0;

    // Heartbeat
    int m_heartbeat_msec = 0;
    std::atomic<bool> m_heartbeat_running{false};
    std::thread m_heartbeat_thread;
    std::thread m_keepalive_thread;

    // Callbacks
    StateCallback m_state_callback;
    SpeakingCallback m_speaking_callback;
    LogCallback m_log_callback;
};

} // namespace dv

#endif // DISCORD_VOICE_CLIENT_H
