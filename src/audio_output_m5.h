// Pipes decoded PCM from arduino-audio-tools into M5.Speaker (ES8311 codec).
// The decoder writes interleaved 16-bit PCM in chunks; we batch into a
// triple-buffer ring and hand each full buffer to playRaw(). Mono / stereo
// handling matches the original ESP8266Audio implementation: stereo down-
// mixed to mono since the speaker is mono.
#pragma once

#include "AudioTools/CoreAudio/AudioOutput.h"
#include <M5Cardputer.h>

class AudioOutputM5CardputerSpeaker : public audio_tools::AudioOutput {
public:
    AudioOutputM5CardputerSpeaker() = default;
    explicit AudioOutputM5CardputerSpeaker(m5::Speaker_Class* spk) : _spk(spk) {}

    // Late-bound speaker pointer; used when the global instance is default-
    // constructed at static init and the actual speaker reference can't be
    // taken until M5Cardputer has been initialised.
    void setSpeaker(m5::Speaker_Class* spk) { _spk = spk; }

    bool begin() override {
        if (!_buf) setRingDepth(DEFAULT_RING_COUNT);
        _running = true;
        return true;
    }
    void end()   override { _running = false; }

    // Reallocate the ring at a new depth. Caller must ensure no audio is
    // playing (audioTask inactive) when this is called — the buffer
    // contents are discarded.
    void setRingDepth(size_t depth) {
        if (depth == _ring_count && _buf) return;
        if (_buf) { delete[] _buf; _buf = nullptr; }
        _ring_count = depth;
        _buf = new int16_t[_ring_count * BUF_SIZE];
        _idx = 0;
        _ring = 0;
    }

    void setAudioInfo(audio_tools::AudioInfo info) override {
        // setAudioInfo is called once per stream start, and again whenever
        // the format changes mid-stream. Log on any actual change.
        if (info.sample_rate != _hertz || info.channels != _channels
            || info.bits_per_sample != _bits) {
            _hertz    = info.sample_rate;
            _channels = info.channels;
            _bits     = info.bits_per_sample;
            Serial.printf("format: rate=%u bits=%u ch=%u\n",
                          (unsigned)_hertz, (unsigned)_bits, (unsigned)_channels);
        }
        audio_tools::AudioOutput::setAudioInfo(info);
    }

    size_t write(const uint8_t* data, size_t len) override {
        if (!_running || _bits != 16 || _channels < 1 || _channels > 2) return len;
        const int16_t* samples = reinterpret_cast<const int16_t*>(data);
        size_t frames = len / (sizeof(int16_t) * _channels);
        for (size_t i = 0; i < frames; ++i) {
            int16_t mono;
            if (_channels == 1) {
                mono = samples[i];
            } else {
                int32_t sum = (int32_t)samples[i * 2] + (int32_t)samples[i * 2 + 1];
                mono = (int16_t)(sum / 2);
            }
            _buf[_ring * BUF_SIZE + _idx++] = mono;
            if (_idx >= BUF_SIZE) flush();
        }
        return len;
    }

    void flush() override {
        if (_idx == 0 || !_spk) return;
        // Underrun condition: speaker drained before we got here.
        bool was_drained = _submitted_once && !_spk->isPlaying();
        if (was_drained) ++_underruns;

        // Submit on channel 0, no preemption. M5Speaker keeps a 2-deep
        // wavinfo queue per channel; if both slots are busy the call
        // blocks inside `_set_next_wav` until one frees. That blocking
        // time is the natural "buffer full" signal — high wait = lots of
        // headroom, low wait = on the edge.
        constexpr int  PLAY_CHANNEL = 0;
        constexpr bool PREEMPT      = false;
        constexpr uint32_t REPEAT   = 1;
        uint32_t t0 = micros();
        _spk->playRaw(&_buf[_ring * BUF_SIZE], _idx, _hertz, false, REPEAT,
                      PLAY_CHANNEL, PREEMPT);
        _last_wait_us = was_drained ? 0 : (micros() - t0);
        _submitted_once = true;
        _ring = (_ring + 1) % _ring_count;
        _idx = 0;
    }

    uint32_t underruns()      const { return _underruns; }
    uint32_t lastWaitMicros() const { return _last_wait_us; }

private:
    // Ring depth governs how much overrun the decoder can put in front of
    // the speaker before the speaker drains back to empty. FLAC frames are
    // 4096–8192 samples and arrive in bursts; 3 slots × 1536 samples drained
    // empty between bursts and underran. 6 slots gives the decoder room.
    // Stored on the heap so depth can be reduced per-track for formats with
    // large decoder workspaces (HE-AAC SBR), reclaiming ~3 KB per slot.
    static constexpr size_t BUF_SIZE = 1536;
    static constexpr size_t DEFAULT_RING_COUNT = 6;
    m5::Speaker_Class* _spk = nullptr;
    int16_t* _buf = nullptr;
    size_t   _ring_count = 0;
    size_t   _idx = 0;
    size_t   _ring = 0;
    uint32_t _underruns = 0;
    uint32_t _last_wait_us = 0;
    bool     _submitted_once = false;
    bool     _running = true;
    uint32_t _hertz = 0;
    uint16_t _channels = 0;
    uint8_t  _bits = 0;
};
