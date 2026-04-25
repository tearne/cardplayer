// Adapted from AndyAiCardputer/mp3-player-winamp-cardputer-adv
// Pipes decoded PCM from ESP8266Audio into M5.Speaker (ES8311 codec).
#pragma once

#include <AudioOutput.h>
#include <M5Cardputer.h>

class AudioOutputM5CardputerSpeaker : public AudioOutput {
public:
    explicit AudioOutputM5CardputerSpeaker(m5::Speaker_Class* spk) : _spk(spk) {}
    bool begin() override { return true; }

    bool SetChannels(int chan) override {
        bool result = AudioOutput::SetChannels(chan);
        // The decoder calls SetChannels(placeholder) from its own begin(),
        // then SetChannels(actual) once a frame is decoded. resetFormatLog()
        // is called between those, so _log_pending is only true for the
        // second (real) call. SetRate is called immediately before this
        // from GetOneSample, so hertz is also up to date.
        if (_log_pending) {
            Serial.printf("format: rate=%u bits=%u ch=%u\n", hertz, bps, channels);
            _log_pending = false;
        }
        return result;
    }

    bool ConsumeSample(int16_t sample[2]) override {
        if (_idx < BUF_SIZE) {
            int16_t mono;
            if (channels == 1) {
                // Mono decoders need not fill sample[1]; using only [0]
                // avoids mixing real audio with whatever happens to be there.
                mono = sample[0];
            } else {
                int32_t sum = (int32_t)sample[0] + (int32_t)sample[1];
                mono = (int16_t)(sum / 2);
            }
            _buf[_ring][_idx++] = mono;
            return true;
        }
        flush();
        return false;
    }

    void flush() override {
        if (_idx == 0) return;
        // If the speaker isn't playing when we arrive, it drained before we
        // refilled — the audio path was starved.
        bool waited = false;
        if (!_spk->isPlaying()) {
            if (_submitted_once) ++_underruns;
            _last_wait_us = 0;
        } else {
            uint32_t t0 = micros();
            uint32_t waited_ticks = 0;
            while (_spk->isPlaying() && waited_ticks++ < 1000) vTaskDelay(1 / portTICK_PERIOD_MS);
            _last_wait_us = micros() - t0;
            waited = true;
        }
        (void)waited;
        _spk->playRaw(_buf[_ring], _idx, hertz, false);
        _submitted_once = true;
        _ring = (_ring + 1) % 3;
        _idx = 0;
    }

    bool stop() override {
        flush();
        while (_spk->isPlaying()) vTaskDelay(1 / portTICK_PERIOD_MS);
        return true;
    }

    uint32_t underruns() const { return _underruns; }
    uint32_t lastWaitMicros() const { return _last_wait_us; }
    void resetFormatLog() { _log_pending = true; }

private:
    static constexpr size_t BUF_SIZE = 1536;
    m5::Speaker_Class* _spk;
    int16_t _buf[3][BUF_SIZE];
    size_t _idx = 0;
    size_t _ring = 0;
    uint32_t _underruns = 0;
    uint32_t _last_wait_us = 0;
    bool _submitted_once = false;
    bool _log_pending = false;
};
