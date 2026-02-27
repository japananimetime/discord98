#include "../include/audio_devices.h"
#include "../deps/miniaudio.h"
#include <cstring>

namespace dv {

AudioDevices::AudioDevices() = default;

void AudioDevices::Enumerate(void *ctx_ptr) {
    auto *ctx = static_cast<ma_context *>(ctx_ptr);
    ma_device_info *pPlayback = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info *pCapture = nullptr;
    ma_uint32 captureCount = 0;

    if (ma_context_get_devices(ctx, &pPlayback, &playbackCount, &pCapture, &captureCount) != MA_SUCCESS) {
        return;
    }

    m_playback_devices.clear();
    m_default_playback.reset();

    for (ma_uint32 i = 0; i < playbackCount; i++) {
        DeviceEntry entry;
        entry.name = pPlayback[i].name;
        entry.index = i;
        entry.is_default = pPlayback[i].isDefault;

        // Store ma_device_id as raw bytes
        entry.device_id_blob.resize(sizeof(ma_device_id));
        std::memcpy(entry.device_id_blob.data(), &pPlayback[i].id, sizeof(ma_device_id));

        if (entry.is_default) {
            m_default_playback = i;
        }
        m_playback_devices.push_back(std::move(entry));
    }

    m_capture_devices.clear();
    m_default_capture.reset();

    for (ma_uint32 i = 0; i < captureCount; i++) {
        DeviceEntry entry;
        entry.name = pCapture[i].name;
        entry.index = i;
        entry.is_default = pCapture[i].isDefault;

        entry.device_id_blob.resize(sizeof(ma_device_id));
        std::memcpy(entry.device_id_blob.data(), &pCapture[i].id, sizeof(ma_device_id));

        if (entry.is_default) {
            m_default_capture = i;
        }
        m_capture_devices.push_back(std::move(entry));
    }
}

const std::vector<DeviceEntry> &AudioDevices::GetPlaybackDevices() const {
    return m_playback_devices;
}

const std::vector<DeviceEntry> &AudioDevices::GetCaptureDevices() const {
    return m_capture_devices;
}

std::optional<uint32_t> AudioDevices::GetDefaultPlaybackIndex() const {
    return m_default_playback;
}

std::optional<uint32_t> AudioDevices::GetDefaultCaptureIndex() const {
    return m_default_capture;
}

const void *AudioDevices::GetMADeviceID(const DeviceEntry &entry) {
    if (entry.device_id_blob.size() != sizeof(ma_device_id)) return nullptr;
    return reinterpret_cast<const void *>(entry.device_id_blob.data());
}

} // namespace dv
