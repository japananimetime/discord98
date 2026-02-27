#include "../include/audio_engine.h"
#include "../deps/miniaudio.h"
#include <opus/opus.h>
#include <algorithm>
#include <cstring>
#include <sstream>

#include "rnnoise.h"

namespace dv {

// miniaudio callbacks (C linkage compatible)
void playback_data_callback(void *pDevice, void *pOutput, const void *pInput, uint32_t frameCount) {
    auto *dev = reinterpret_cast<ma_device *>(pDevice);
    auto *mgr = reinterpret_cast<AudioEngine *>(dev->pUserData);
    if (mgr) mgr->OnPlaybackRequested(static_cast<float *>(pOutput), frameCount);
}

void capture_data_callback(void *pDevice, void *pOutput, const void *pInput, uint32_t frameCount) {
    auto *dev = reinterpret_cast<ma_device *>(pDevice);
    auto *mgr = reinterpret_cast<AudioEngine *>(dev->pUserData);
    if (mgr) {
        mgr->OnCapturedPCM(static_cast<const int16_t *>(pInput), frameCount);
        mgr->m_rtp_timestamp += 480;
    }
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    Shutdown();
}

bool AudioEngine::Init() {
    if (m_initialized) return m_ok;
    m_initialized = true;

    // Create Opus encoder
    int err;
    m_encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        Log(LOG_ERROR, "Failed to create Opus encoder: " + std::to_string(err));
        m_ok = false;
        return false;
    }
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(64000));

    // Create miniaudio context
    auto *ctx = new ma_context;
    auto ctx_cfg = ma_context_config_init();
    if (ma_context_init(nullptr, 0, &ctx_cfg, ctx) != MA_SUCCESS) {
        Log(LOG_ERROR, "Failed to initialize audio context");
        delete ctx;
        m_ok = false;
        return false;
    }
    m_context_ptr = ctx;

    // Enumerate devices
    m_devices.Enumerate(ctx);

    // Initialize playback device
    auto *playback = new ma_device;
    auto playback_cfg = ma_device_config_init(ma_device_type_playback);
    playback_cfg.playback.format = ma_format_f32;
    playback_cfg.playback.channels = 2;
    playback_cfg.sampleRate = 48000;
    playback_cfg.dataCallback = reinterpret_cast<ma_device_data_proc>(playback_data_callback);
    playback_cfg.pUserData = this;

    if (auto def = m_devices.GetDefaultPlaybackIndex()) {
        auto &devs = m_devices.GetPlaybackDevices();
        auto *dev_id = static_cast<const ma_device_id *>(AudioDevices::GetMADeviceID(devs[*def]));
        if (dev_id) {
            playback_cfg.playback.pDeviceID = dev_id;
        }
    }

    if (ma_device_init(ctx, &playback_cfg, playback) != MA_SUCCESS) {
        Log(LOG_ERROR, "Failed to initialize playback device");
        delete playback;
        m_ok = false;
        return false;
    }

    if (ma_device_start(playback) != MA_SUCCESS) {
        Log(LOG_ERROR, "Failed to start playback device");
        ma_device_uninit(playback);
        delete playback;
        m_ok = false;
        return false;
    }

    m_playback_ptr = playback;
    Log(LOG_INFO, "Playback device started");

    // Initialize capture device (don't start yet — will start when joining voice)
    auto *capture = new ma_device;
    auto capture_cfg = ma_device_config_init(ma_device_type_capture);
    capture_cfg.capture.format = ma_format_s16;
    capture_cfg.capture.channels = 2;
    capture_cfg.sampleRate = 48000;
    capture_cfg.periodSizeInFrames = 480;
    capture_cfg.dataCallback = reinterpret_cast<ma_device_data_proc>(capture_data_callback);
    capture_cfg.pUserData = this;

    if (auto def = m_devices.GetDefaultCaptureIndex()) {
        auto &devs = m_devices.GetCaptureDevices();
        auto *dev_id = static_cast<const ma_device_id *>(AudioDevices::GetMADeviceID(devs[*def]));
        if (dev_id) {
            capture_cfg.capture.pDeviceID = dev_id;
        }
    }

    if (ma_device_init(ctx, &capture_cfg, capture) != MA_SUCCESS) {
        Log(LOG_ERROR, "Failed to initialize capture device");
        delete capture;
        // Capture failure is non-fatal (you can still listen)
    } else {
        m_capture_ptr = capture;
        Log(LOG_INFO, "Capture device initialized");
    }

    // Create RNNoise denoiser state
    m_denoiser = rnnoise_create();
    if (!m_denoiser) {
        Log(LOG_WARN, "Failed to create RNNoise denoiser");
    } else {
        Log(LOG_INFO, "RNNoise denoiser initialized");
    }

    m_ok = true;
    return true;
}

void AudioEngine::Shutdown() {
    if (!m_initialized) return;

    if (m_playback_ptr) {
        auto *dev = static_cast<ma_device *>(m_playback_ptr);
        ma_device_uninit(dev);
        delete dev;
        m_playback_ptr = nullptr;
    }
    if (m_capture_ptr) {
        auto *dev = static_cast<ma_device *>(m_capture_ptr);
        ma_device_uninit(dev);
        delete dev;
        m_capture_ptr = nullptr;
    }
    if (m_context_ptr) {
        auto *ctx = static_cast<ma_context *>(m_context_ptr);
        ma_context_uninit(ctx);
        delete ctx;
        m_context_ptr = nullptr;
    }

    RemoveAllSSRCs();

    if (m_denoiser) {
        rnnoise_destroy(static_cast<DenoiseState *>(m_denoiser));
        m_denoiser = nullptr;
    }

    if (m_encoder) {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }

    m_initialized = false;
    m_ok = false;
}

bool AudioEngine::IsOK() const {
    return m_ok;
}

AudioDevices &AudioEngine::GetDevices() {
    return m_devices;
}

void AudioEngine::SetPlaybackDevice(uint32_t device_index) {
    auto &devs = m_devices.GetPlaybackDevices();
    if (device_index >= devs.size() || !m_context_ptr) return;

    auto *ctx = static_cast<ma_context *>(m_context_ptr);
    auto *dev_id = static_cast<const ma_device_id *>(AudioDevices::GetMADeviceID(devs[device_index]));
    if (!dev_id) return;

    if (m_playback_ptr) {
        auto *old_dev = static_cast<ma_device *>(m_playback_ptr);
        ma_device_uninit(old_dev);
        delete old_dev;
    }

    auto *playback = new ma_device;
    auto cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.playback.pDeviceID = dev_id;
    cfg.sampleRate = 48000;
    cfg.dataCallback = reinterpret_cast<ma_device_data_proc>(playback_data_callback);
    cfg.pUserData = this;

    if (ma_device_init(ctx, &cfg, playback) != MA_SUCCESS) {
        Log(LOG_ERROR, "Failed to init new playback device");
        delete playback;
        m_playback_ptr = nullptr;
        return;
    }
    if (ma_device_start(playback) != MA_SUCCESS) {
        Log(LOG_ERROR, "Failed to start new playback device");
        ma_device_uninit(playback);
        delete playback;
        m_playback_ptr = nullptr;
        return;
    }
    m_playback_ptr = playback;
    Log(LOG_INFO, "Switched playback device to: " + devs[device_index].name);
}

void AudioEngine::SetCaptureDevice(uint32_t device_index) {
    auto &devs = m_devices.GetCaptureDevices();
    if (device_index >= devs.size() || !m_context_ptr) return;

    auto *ctx = static_cast<ma_context *>(m_context_ptr);
    auto *dev_id = static_cast<const ma_device_id *>(AudioDevices::GetMADeviceID(devs[device_index]));
    if (!dev_id) return;

    if (m_capture_ptr) {
        auto *old_dev = static_cast<ma_device *>(m_capture_ptr);
        ma_device_uninit(old_dev);
        delete old_dev;
    }

    auto *capture = new ma_device;
    auto cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_s16;
    cfg.capture.channels = 2;
    cfg.capture.pDeviceID = dev_id;
    cfg.sampleRate = 48000;
    cfg.periodSizeInFrames = 480;
    cfg.dataCallback = reinterpret_cast<ma_device_data_proc>(capture_data_callback);
    cfg.pUserData = this;

    if (ma_device_init(ctx, &cfg, capture) != MA_SUCCESS) {
        Log(LOG_ERROR, "Failed to init new capture device");
        delete capture;
        m_capture_ptr = nullptr;
        return;
    }
    if (ma_device_start(capture) != MA_SUCCESS) {
        Log(LOG_ERROR, "Failed to start new capture device");
        ma_device_uninit(capture);
        delete capture;
        m_capture_ptr = nullptr;
        return;
    }
    m_capture_ptr = capture;
    Log(LOG_INFO, "Switched capture device to: " + devs[device_index].name);
}

void AudioEngine::StartPlayback() {
    if (m_playback_ptr) {
        auto *dev = static_cast<ma_device *>(m_playback_ptr);
        if (ma_device_get_state(dev) != ma_device_state_started) {
            ma_device_start(dev);
        }
    }
}

void AudioEngine::StopPlayback() {
    if (m_playback_ptr) {
        auto *dev = static_cast<ma_device *>(m_playback_ptr);
        ma_device_stop(dev);
    }
}

void AudioEngine::StartCapture() {
    if (m_capture_ptr) {
        auto *dev = static_cast<ma_device *>(m_capture_ptr);
        if (ma_device_get_state(dev) != ma_device_state_started) {
            if (ma_device_start(dev) != MA_SUCCESS) {
                Log(LOG_ERROR, "Failed to start capture");
            } else {
                Log(LOG_INFO, "Capture started");
            }
        }
    }
}

void AudioEngine::StopCapture() {
    if (m_capture_ptr) {
        auto *dev = static_cast<ma_device *>(m_capture_ptr);
        ma_device_stop(dev);
        Log(LOG_INFO, "Capture stopped");
    }
}

void AudioEngine::SetCaptureEnabled(bool enabled) {
    m_capture_enabled = enabled;
}

void AudioEngine::SetPlaybackEnabled(bool enabled) {
    m_playback_enabled = enabled;
}

// --- SSRC management ---

void AudioEngine::AddSSRC(uint32_t ssrc) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_sources.find(ssrc) == m_sources.end()) {
        int error;
        auto *decoder = opus_decoder_create(48000, 2, &error);
        if (error != OPUS_OK) {
            Log(LOG_ERROR, "Failed to create Opus decoder for SSRC " + std::to_string(ssrc));
            return;
        }
        SSRCSource src;
        src.decoder = decoder;
        m_sources[ssrc] = std::move(src);
    }
}

void AudioEngine::RemoveSSRC(uint32_t ssrc) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (auto it = m_sources.find(ssrc); it != m_sources.end()) {
        opus_decoder_destroy(it->second.decoder);
        m_sources.erase(it);
    }
}

void AudioEngine::RemoveAllSSRCs() {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto &[ssrc, src] : m_sources) {
        if (src.decoder) opus_decoder_destroy(src.decoder);
    }
    m_sources.clear();
}

void AudioEngine::FeedMeOpus(uint32_t ssrc, const std::vector<uint8_t> &data) {
    if (!m_playback_enabled) return;
    if (!m_playback_ptr) return;

    auto *dev = static_cast<ma_device *>(m_playback_ptr);
    if (ma_device_get_state(dev) != ma_device_state_started) return;

    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_muted_ssrcs.count(ssrc)) return;

    opus_int16 pcm[120 * 48 * 2]; // Max frame size
    if (auto it = m_sources.find(ssrc); it != m_sources.end()) {
        int decoded = opus_decode(it->second.decoder, data.data(),
                                  static_cast<opus_int32>(data.size()),
                                  pcm, 120 * 48, 0);
        if (decoded > 0) {
            UpdateReceiveVolume(ssrc, pcm, decoded);
            auto &buf = it->second.buffer;
            buf.insert(buf.end(), pcm, pcm + decoded * 2);
        }
    }
}

// --- Volume controls ---

void AudioEngine::SetCaptureGain(double gain) { m_capture_gain = gain; }
double AudioEngine::GetCaptureGain() const noexcept { return m_capture_gain; }
void AudioEngine::SetCaptureGate(double gate) { m_capture_gate = gate; }
double AudioEngine::GetCaptureGate() const noexcept { return m_capture_gate; }
void AudioEngine::SetPlaybackGain(double gain) { m_playback_gain = gain; }
double AudioEngine::GetPlaybackGain() const noexcept { return m_playback_gain; }
void AudioEngine::SetNoiseSuppress(bool enabled) { m_noise_suppress = enabled; }
bool AudioEngine::GetNoiseSuppress() const noexcept { return m_noise_suppress; }

void AudioEngine::SetMuteSSRC(uint32_t ssrc, bool mute) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (mute) m_muted_ssrcs.insert(ssrc);
    else m_muted_ssrcs.erase(ssrc);
}

void AudioEngine::SetVolumeSSRC(uint32_t ssrc, double volume) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_volume_ssrc[ssrc] = volume;
}

double AudioEngine::GetVolumeSSRC(uint32_t ssrc) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (auto it = m_volume_ssrc.find(ssrc); it != m_volume_ssrc.end()) return it->second;
    return 1.0;
}

void AudioEngine::SetBitrate(int bitrate) {
    std::lock_guard<std::mutex> lk(m_enc_mutex);
    if (m_encoder) opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bitrate));
}

int AudioEngine::GetBitrate() {
    std::lock_guard<std::mutex> lk(m_enc_mutex);
    int temp = 64000;
    if (m_encoder) opus_encoder_ctl(m_encoder, OPUS_GET_BITRATE(&temp));
    return temp;
}

uint32_t AudioEngine::GetRTPTimestamp() const noexcept {
    return m_rtp_timestamp;
}

double AudioEngine::GetCaptureVolumeLevel() const noexcept {
    return m_capture_peak_meter / 32768.0;
}

double AudioEngine::GetSSRCVolumeLevel(uint32_t ssrc) const noexcept {
    std::lock_guard<std::mutex> lk(m_vol_mtx);
    if (auto it = m_volumes.find(ssrc); it != m_volumes.end()) return it->second;
    return 0.0;
}

void AudioEngine::SetMixMono(bool value) { m_mix_mono = value; }
bool AudioEngine::GetMixMono() const { return m_mix_mono; }

void AudioEngine::SetOpusPacketCallback(OpusPacketCallback cb) {
    m_opus_packet_callback = std::move(cb);
}

void AudioEngine::SetLogCallback(LogCallback cb) {
    m_log_callback = std::move(cb);
}

// --- miniaudio callbacks ---

void AudioEngine::OnPlaybackRequested(float *pOutput, uint32_t frameCount) {
    const double playback_gain = m_playback_gain;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto &[ssrc, src] : m_sources) {
        double volume = playback_gain;
        if (auto it = m_volume_ssrc.find(ssrc); it != m_volume_ssrc.end()) {
            volume *= it->second;
        }
        auto &buf = src.buffer;
        const size_t n = std::min(static_cast<size_t>(buf.size()),
                                  static_cast<size_t>(frameCount * 2ULL));
        for (size_t i = 0; i < n; i++) {
            pOutput[i] += static_cast<float>(volume * buf[i] / 32768.0);
        }
        buf.erase(buf.begin(), buf.begin() + n);
    }
}

void AudioEngine::OnCapturedPCM(const int16_t *pcm, uint32_t frames) {
    if (!m_encoder || !m_capture_enabled) return;

    const double gain = m_capture_gain;

    // Apply gain
    std::vector<int16_t> processed(pcm, pcm + frames * 2);
    for (auto &val : processed) {
        int32_t unclamped = static_cast<int32_t>(val * gain);
        val = static_cast<int16_t>(std::clamp(unclamped, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
    }

    // Optional mono mix
    if (m_mix_mono) {
        for (size_t i = 0; i < frames * 2; i += 2) {
            int16_t mixed = static_cast<int16_t>((processed[i] + processed[i + 1]) / 2);
            processed[i] = mixed;
            processed[i + 1] = mixed;
        }
    }

    // Apply RNNoise noise suppression (mono, before encoding)
    if (m_noise_suppress && m_denoiser && frames == 480) {
        // Mix to mono for denoising
        float mono_float[480];
        for (uint32_t i = 0; i < 480; i++) {
            mono_float[i] = static_cast<float>((processed[i * 2] + processed[i * 2 + 1]) / 2);
        }
        // RNNoise expects float input scaled to [-32768, 32768]
        float denoised[480];
        rnnoise_process_frame(static_cast<DenoiseState *>(m_denoiser), denoised, mono_float);
        // Write denoised mono back to stereo
        for (uint32_t i = 0; i < 480; i++) {
            int16_t val = static_cast<int16_t>(std::clamp(static_cast<int32_t>(denoised[i]), (int32_t)INT16_MIN, (int32_t)INT16_MAX));
            processed[i * 2] = val;
            processed[i * 2 + 1] = val;
        }
    }

    UpdateCaptureVolume(processed.data(), frames);

    // Voice activity detection (simple gate)
    if (!CheckVADVoiceGate()) return;

    // Encode to Opus
    std::lock_guard<std::mutex> lk(m_enc_mutex);
    int payload_len = opus_encode(m_encoder, processed.data(), 480,
                                  m_opus_buffer.data(), static_cast<opus_int32>(m_opus_buffer.size()));
    if (payload_len > 0 && m_opus_packet_callback) {
        m_opus_packet_callback(m_opus_buffer.data(), payload_len);
    }
}

bool AudioEngine::CheckVADVoiceGate() {
    return m_capture_peak_meter / 32768.0 > m_capture_gate;
}

void AudioEngine::UpdateCaptureVolume(const int16_t *pcm, uint32_t frames) {
    for (uint32_t i = 0; i < frames * 2; i += 2) {
        int amp = std::abs(pcm[i]);
        int current = m_capture_peak_meter.load(std::memory_order_relaxed);
        if (amp > current) m_capture_peak_meter = amp;
    }
}

void AudioEngine::UpdateReceiveVolume(uint32_t ssrc, const int16_t *pcm, int frames) {
    std::lock_guard<std::mutex> lk(m_vol_mtx);
    auto &meter = m_volumes[ssrc];
    for (int i = 0; i < frames * 2; i += 2) {
        double amp = std::abs(pcm[i]) / 32768.0;
        meter = std::max(meter, amp);
    }
}

void AudioEngine::Log(int level, const std::string &msg) {
    if (m_log_callback) m_log_callback(level, "[AudioEngine] " + msg);
}

} // namespace dv
