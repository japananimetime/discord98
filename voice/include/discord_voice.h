#pragma once
#ifndef DISCORD_VOICE_H
#define DISCORD_VOICE_H

// Discord Voice Library - Standalone voice chat for Discord
// Extracted and adapted from Abaddon (https://github.com/uowuo/abaddon)
// No GTK/GLib dependencies - pure C++ with Win32 support
//
// Dependencies:
//   - libopus    (audio codec)
//   - libsodium  (encryption)
//   - miniaudio  (audio I/O, header-only)
//   - nlohmann/json (JSON, header-only)
//
// Usage:
//   1. Implement IVoiceWebSocket using your preferred WebSocket library
//   2. Create AudioEngine, call Init()
//   3. Create VoiceClient, set WebSocket + AudioEngine
//   4. Set VoiceServerInfo (endpoint, token, session_id, server_id, user_id)
//   5. Call VoiceClient::Start()
//
// The library handles:
//   - Voice gateway WebSocket protocol (opcodes, heartbeat, etc.)
//   - UDP transport with IP discovery
//   - AEAD XChaCha20-Poly1305 encryption
//   - Opus encode/decode
//   - Microphone capture and speaker playback via WASAPI (on Windows)

#include "voice_types.h"
#include "voice_client.h"
#include "audio_engine.h"
#include "audio_devices.h"
#include "udp_socket.h"

#endif // DISCORD_VOICE_H
