#pragma once
#ifndef DISCORD_VOICE_AUDIO_DEVICES_H
#define DISCORD_VOICE_AUDIO_DEVICES_H

#include <optional>
#include <string>
#include <vector>
#include "voice_types.h"

namespace dv {

struct DeviceEntry {
    std::string name;
    uint32_t index = 0;
    bool is_default = false;
    // Opaque storage for ma_device_id (avoid exposing miniaudio in header)
    std::vector<uint8_t> device_id_blob;
};

class AudioDevices {
public:
    AudioDevices();

    // ctx is a ma_context* passed as void* to avoid miniaudio.h in header
    void Enumerate(void *ctx);

    const std::vector<DeviceEntry> &GetPlaybackDevices() const;
    const std::vector<DeviceEntry> &GetCaptureDevices() const;

    std::optional<uint32_t> GetDefaultPlaybackIndex() const;
    std::optional<uint32_t> GetDefaultCaptureIndex() const;

    // Get the raw ma_device_id pointer (void*) from a DeviceEntry
    // Returns nullptr if blob is wrong size
    static const void *GetMADeviceID(const DeviceEntry &entry);

private:
    std::vector<DeviceEntry> m_playback_devices;
    std::vector<DeviceEntry> m_capture_devices;
    std::optional<uint32_t> m_default_playback;
    std::optional<uint32_t> m_default_capture;
};

} // namespace dv

#endif // DISCORD_VOICE_AUDIO_DEVICES_H
