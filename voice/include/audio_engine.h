#pragma once
#ifndef DISCORD_VOICE_AUDIO_ENGINE_H
#define DISCORD_VOICE_AUDIO_ENGINE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "voice_types.h"
#include "audio_devices.h"

// Forward declarations - Opus types are structs
struct OpusEncoder;
struct OpusDecoder;

namespace dv {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool Init();
    void Shutdown();
    bool IsOK() const;

    // Device management
    AudioDevices &GetDevices();
    void SetPlaybackDevice(uint32_t device_index);
    void SetCaptureDevice(uint32_t device_index);

    // Playback control
    void StartPlayback();
    void StopPlayback();

    // Capture control
    void StartCapture();
    void StopCapture();

    void SetCaptureEnabled(bool enabled);
    void SetPlaybackEnabled(bool enabled);

    // SSRC management (per-user audio streams)
    void AddSSRC(uint32_t ssrc);
    void RemoveSSRC(uint32_t ssrc);
    void RemoveAllSSRCs();

    // Feed received Opus data for playback
    void FeedMeOpus(uint32_t ssrc, const std::vector<uint8_t> &data);

    // Volume controls
    void SetCaptureGain(double gain);
    double GetCaptureGain() const noexcept;
    void SetCaptureGate(double gate);
    double GetCaptureGate() const noexcept;
    void SetPlaybackGain(double gain);
    double GetPlaybackGain() const noexcept;

    // Noise suppression (RNNoise)
    void SetNoiseSuppress(bool enabled);
    bool GetNoiseSuppress() const noexcept;

    void SetMuteSSRC(uint32_t ssrc, bool mute);
    void SetVolumeSSRC(uint32_t ssrc, double volume);
    double GetVolumeSSRC(uint32_t ssrc) const;

    // Opus encoder settings
    void SetBitrate(int bitrate);
    int GetBitrate();

    // RTP timestamp
    uint32_t GetRTPTimestamp() const noexcept;

    // Volume metering
    double GetCaptureVolumeLevel() const noexcept;
    double GetSSRCVolumeLevel(uint32_t ssrc) const noexcept;

    void SetMixMono(bool value);
    bool GetMixMono() const;

    // Callback: called when an Opus packet is encoded and ready to send
    using OpusPacketCallback = std::function<void(const uint8_t *data, int size)>;
    void SetOpusPacketCallback(OpusPacketCallback cb);

    void SetLogCallback(LogCallback cb);

private:
    // Called by miniaudio
    friend void playback_data_callback(void *pDevice, void *pOutput, const void *pInput, uint32_t frameCount);
    friend void capture_data_callback(void *pDevice, void *pOutput, const void *pInput, uint32_t frameCount);

    void OnCapturedPCM(const int16_t *pcm, uint32_t frames);
    void OnPlaybackRequested(float *pOutput, uint32_t frameCount);

    bool CheckVADVoiceGate();
    void UpdateCaptureVolume(const int16_t *pcm, uint32_t frames);
    void UpdateReceiveVolume(uint32_t ssrc, const int16_t *pcm, int frames);

    void Log(int level, const std::string &msg);

    bool m_ok = false;
    bool m_initialized = false;

    // miniaudio - stored as opaque blobs to avoid including miniaudio.h
    void *m_context_ptr = nullptr;    // ma_context*
    void *m_playback_ptr = nullptr;   // ma_device*
    void *m_capture_ptr = nullptr;    // ma_device*

    mutable std::mutex m_mutex;
    mutable std::mutex m_enc_mutex;

    // Per-SSRC: decoded PCM buffer + Opus decoder
    struct SSRCSource {
        std::deque<int16_t> buffer;
        OpusDecoder *decoder = nullptr;
    };
    std::unordered_map<uint32_t, SSRCSource> m_sources;

    // Opus encoder
    OpusEncoder *m_encoder = nullptr;
    std::array<uint8_t, 1275> m_opus_buffer{};

    // State
    std::atomic<bool> m_capture_enabled{true};
    std::atomic<bool> m_playback_enabled{true};
    std::atomic<double> m_capture_gate{0.0};
    std::atomic<double> m_capture_gain{1.0};
    std::atomic<double> m_playback_gain{1.0};
    std::atomic<bool> m_noise_suppress{false};
    std::atomic<bool> m_mix_mono{false};

    // RNNoise denoiser state (opaque to avoid rnnoise.h in header)
    void *m_denoiser = nullptr;
    std::atomic<int> m_capture_peak_meter{0};

    std::unordered_set<uint32_t> m_muted_ssrcs;
    std::unordered_map<uint32_t, double> m_volume_ssrc;

    mutable std::mutex m_vol_mtx;
    std::unordered_map<uint32_t, double> m_volumes;

    std::atomic<uint32_t> m_rtp_timestamp{0};

    AudioDevices m_devices;

    OpusPacketCallback m_opus_packet_callback;
    LogCallback m_log_callback;
};

} // namespace dv

#endif // DISCORD_VOICE_AUDIO_ENGINE_H
