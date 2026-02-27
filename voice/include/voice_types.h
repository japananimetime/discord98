#pragma once
#ifndef DISCORD_VOICE_TYPES_H
#define DISCORD_VOICE_TYPES_H

#include <cstdint>
#include <string>
#include <array>
#include <vector>
#include <functional>

namespace dv {

using Snowflake = uint64_t;

enum class VoiceGatewayOp : int {
    Identify = 0,
    SelectProtocol = 1,
    Ready = 2,
    Heartbeat = 3,
    SessionDescription = 4,
    Speaking = 5,
    HeartbeatAck = 6,
    Resume = 7,
    Hello = 8,
    Resumed = 9,
    Video = 12,
    ClientDisconnect = 13,
    SessionUpdate = 14,
    MediaSinkWants = 15,
    VoiceBackendVersion = 16,
};

enum class VoiceGatewayCloseCode : uint16_t {
    Normal = 4000,
    UnknownOpcode = 4001,
    InvalidPayload = 4002,
    NotAuthenticated = 4003,
    AuthenticationFailed = 4004,
    AlreadyAuthenticated = 4005,
    SessionInvalid = 4006,
    SessionTimedOut = 4009,
    ServerNotFound = 4011,
    UnknownProtocol = 4012,
    Disconnected = 4014,
    ServerCrashed = 4015,
    UnknownEncryption = 4016,
};

enum class SpeakingFlags : uint32_t {
    None = 0,
    Microphone = 1 << 0,
    Soundshare = 1 << 1,
    Priority = 1 << 2,
};

enum class VoiceState {
    Disconnected,
    Connecting,
    Establishing,
    Connected,
};

struct VoiceServerInfo {
    std::string endpoint;
    std::string token;
    std::string session_id;
    Snowflake server_id = 0;
    Snowflake user_id = 0;
    bool video = false;
};

struct VoiceReadyInfo {
    std::string ip;
    uint16_t port = 0;
    uint32_t ssrc = 0;
    std::vector<std::string> modes;
};

struct SpeakingInfo {
    Snowflake user_id = 0;
    uint32_t ssrc = 0;
    SpeakingFlags speaking = SpeakingFlags::None;
};

struct AudioDeviceInfo {
    std::string name;
    uint32_t index = 0;
    bool is_default = false;
};

// Callbacks
using StateCallback = std::function<void(VoiceState state)>;
using SpeakingCallback = std::function<void(const SpeakingInfo &info)>;
using LogCallback = std::function<void(int level, const std::string &msg)>;

enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
};

} // namespace dv

#endif // DISCORD_VOICE_TYPES_H
