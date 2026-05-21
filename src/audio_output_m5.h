// Pipes decoded PCM from ESP8266Audio into M5.Speaker (ES8311 codec).
// Stereo down-mixed to mono since the speaker is mono. A pre-buffer
// between ConsumeSample and the speaker queue lets the decoder run ahead
// of audible playback by ~150 ms so the waveform visualisation has
// meaningful look-ahead. The speaker's own 2-deep wavinfo queue is still
// the rate-limiter; the pre-buffer just decouples decoder pacing from
// speaker submission.
#pragma once

#include <AudioOutput.h>
#include <M5Cardputer.h>

#include <cstring>

#include <arduinoFFT.h>

// Spectrum tap — 256-pt real FFT once per 256 incoming mono samples.
// Magnitudes log-grouped into SPEC_BINS visible bands and accumulated into
// the in-progress display column. Column commits at SPEC_COL_SAMPLES
// boundaries, advancing a column ring read by the heatmap render path.
static constexpr int      SPEC_FFT_SIZE     = 256;
static constexpr int      SPEC_BINS         = 28;
// Ring is wider than the screen (240 px) so the audio task's writer and
// the UI task's reader can never touch the same slot. With SPEC_COLS ==
// SCREEN_W there's no headroom: a 1-col lag in disp_abs caused the
// latest write to wrap into x=0. Doubling the ring puts 240 cols of
// margin between writer and reader.
static constexpr int      SPEC_COLS         = 256;
// Visualisation tuning.
//
// `VIZ_ZOOM_SECONDS` is the user-facing knob — how long a span of audio
// is visible on screen. Everything else derives from it (here and in
// main.cpp). The relationship:
//
//   display_cols_per_sec = SCREEN_W / ZOOM_SECONDS
//   SPEC_COL_SAMPLES     = sample_rate / display_cols_per_sec
//   cols_per_render      = display_cols_per_sec / panel_scan_hz  (must be int)
//
// For ZOOM = 2.0 at 44.1 kHz / 60 Hz panel: 120 cols/sec, 368 samples/col,
// 2 cols per render. Floor on SPEC_COL_SAMPLES is SPEC_FFT_SIZE (256) —
// below that, commits outrun FFTs.
static constexpr float    VIZ_ZOOM_SECONDS  = 4.0f;
static constexpr int      VIZ_SCREEN_COLS   = 240;
static constexpr float    VIZ_COLS_PER_SEC  = VIZ_SCREEN_COLS / VIZ_ZOOM_SECONDS;
static constexpr uint32_t SPEC_COL_SAMPLES  =
    (uint32_t)(44100.0f / VIZ_COLS_PER_SEC + 0.5f);

struct SpectrumRing {
    uint8_t  intensity[SPEC_COLS][SPEC_BINS] = {};
    volatile uint32_t head     = 0;  // next column to write
    volatile uint64_t abs_head = 0;  // monotonic count
};

// Waveform ring tap — per-column min/max peaks of the most recently
// decoded mono samples. Audio task writes; UI task reads. Per-cell
// update is non-atomic but the visible glitch is one column wide at
// most — acceptable for visualisation.
// Wider than screen (= 240 px) so the visible 240-col viewport has room
// to slide over the buffer without falling off the ends. New cols still
// arrive at audio rate; the viewport position is driven by wall-clock
// time so display scroll stays smooth even when the data head jitters.
static constexpr int WV_COLS = 256;
struct WaveformRing {
    int8_t  min_v[WV_COLS] = {};
    int8_t  max_v[WV_COLS] = {};
    volatile uint32_t head = 0;  // next column to write (modular index)
};

class AudioOutputM5CardputerSpeaker : public AudioOutput {
public:
    AudioOutputM5CardputerSpeaker() = default;
    explicit AudioOutputM5CardputerSpeaker(m5::Speaker_Class* spk) : _spk(spk) {}

    void setSpeaker(m5::Speaker_Class* spk) { _spk = spk; }

    const WaveformRing& waveformRing() const { return _wv; }
    const SpectrumRing& spectrumRing() const { return _spec; }

    // One-time FFT init. Safe to call before begin(); idempotent.
    void initSpectrum() {
        if (_spec_inited) return;
        _spec_fft = new ArduinoFFT<float>(_spec_real, _spec_imag,
                                          SPEC_FFT_SIZE, 44100.0f);
        _spec_inited = true;
    }

    // Waveform commits at the same sample cadence as the spectrum so
    // both rings advance in lockstep and a single display cursor can
    // drive both views. The lookahead-driven calibration that used to
    // live here is gone with the playhead-at-20 % design.
    int waveformSamplesPerCol() const { return SPEC_COL_SAMPLES; }

    uint32_t sampleRate() const { return hertz ? hertz : 44100; }

    bool begin() override { return true; }

    bool SetChannels(int chan) override {
        bool result = AudioOutput::SetChannels(chan);
        if (_log_pending) {
            Serial.printf("format: rate=%u bits=%u ch=%u\n", hertz, bps, channels);
            _log_pending = false;
        }
        return result;
    }

    // ConsumeSample now writes to the pre-buffer (a flat circular ring of
    // int16 mono samples). Returns false when the pre-buffer is full; the
    // decoder retries — which yields control so the audio task can run
    // shipBuffered() in the gap.
    bool ConsumeSample(int16_t sample[2]) override {
        if (!_spk) return false;
        if (_prebuf_count >= PREBUF_SAMPLES) {
            return false;  // pre-buffer full — caller retries after shipping
        }

        int16_t mono;
        if (channels == 1) {
            mono = sample[0];
        } else {
            int32_t sum = (int32_t)sample[0] + (int32_t)sample[1];
            mono = (int16_t)(sum / 2);
        }
        _prebuf[_prebuf_head] = mono;
        _prebuf_head = (_prebuf_head + 1) % PREBUF_SAMPLES;
        _prebuf_count++;
        _samples_consumed++;

        // Waveform ring update — same shape as before but tapping
        // pre-buffer ingress (= deepest available future).
        if (mono < _wv_min) _wv_min = mono;
        if (mono > _wv_max) _wv_max = mono;
        // Visualisation taps (waveform + spectrum) are suppressed for a
        // window after resetVisualisation so the decoder's pre-seek
        // sample pipeline drains without contaminating either ring.
        // Both taps gated by the same counter so the two views stay
        // in lockstep across a reset.
        if (_viz_suppress_samples > 0) {
            _viz_suppress_samples--;
            return true;
        }

        if (++_wv_count >= waveformSamplesPerCol()) {
            uint32_t h = _wv.head;
            _wv.min_v[h] = (int8_t)(_wv_min >> 8);
            _wv.max_v[h] = (int8_t)(_wv_max >> 8);
            _wv.head = (h + 1) % WV_COLS;
            _wv_min = INT16_MAX;
            _wv_max = INT16_MIN;
            _wv_count = 0;
        }

        if (_spec_inited) {
            _spec_samples[_spec_fill++] = (float)mono * (1.0f / 32768.0f);
            if (_spec_fill == SPEC_FFT_SIZE) {
                computeSpectrum();
                _spec_fill = 0;
            }
            if (++_spec_col_samples >= SPEC_COL_SAMPLES) {
                commitSpectrumColumn();
                _spec_col_samples = 0;
            }
        }
        return true;
    }

    // Submit one slot's worth of pre-buffered samples to the speaker if
    // we have at least that many ready. Called from the audio task on
    // every iteration. Blocks inside playRaw when speaker's queue is
    // full — that's the rate-limiter, same as the original design.
    void shipBuffered() {
        if (!_spk) return;
        if (_prebuf_count < BUF_SIZE) return;
        copyAndSubmit(BUF_SIZE);
    }

    // Submit whatever's left in the pre-buffer, even a partial slot.
    // Used on stop / track change.
    void flush() override {
        if (_prebuf_count == 0 || !_spk) return;
        size_t n = _prebuf_count;
        if (n > BUF_SIZE) n = BUF_SIZE;
        copyAndSubmit(n);
        if (_prebuf_count >= BUF_SIZE) {
            // Pre-buffer had more than one slot's worth left — drain
            // remainder so stop/skip empties the path.
            copyAndSubmit(BUF_SIZE);
        }
    }

    bool stop() override {
        flush();
        while (_spk && _spk->isPlaying()) vTaskDelay(1 / portTICK_PERIOD_MS);
        return true;
    }

    // Discard everything in flight from decoder to speaker. Used on
    // stop / skip so the user doesn't hear up to ~220 ms of stale audio
    // after pressing the key.
    void hardFlush() {
        _prebuf_head  = 0;
        _prebuf_tail  = 0;
        _prebuf_count = 0;
        // Visualisation rings deliberately left intact — the on-screen
        // heatmap / waveform scrolls continuously across jumps. Call
        // `resetVisualisation` separately if a true blank is desired.
        if (_spk) _spk->stop();
    }

    // Wipe both visualisation rings without touching the pre-buffer or
    // speaker. Used on intra-track seeks where we want the overlays to
    // blank but audio to continue from its existing pre-buffer. Also
    // suppresses tap updates for ~200 ms so pre-seek audio still sitting
    // in the decoder's internal buffers doesn't seed the rings with
    // stale content as it flows through ConsumeSample.
    void resetVisualisation() {
        memset(_wv.min_v, 0, sizeof(_wv.min_v));
        memset(_wv.max_v, 0, sizeof(_wv.max_v));
        _wv_min = INT16_MAX;
        _wv_max = INT16_MIN;
        _wv_count = 0;
        memset(_spec.intensity, 0, sizeof(_spec.intensity));
        for (int b = 0; b < SPEC_BINS; b++) _spec_accum[b] = 0.0f;
        _spec_fill = 0;
        _spec_col_samples = 0;
        _viz_suppress_samples = sampleRate() / 5;  // ~200 ms
    }

    uint32_t underruns()      const { return _underruns; }
    uint32_t lastWaitMicros() const { return _last_wait_us; }
    size_t   prebufSamples()  const { return _prebuf_count; }
    size_t   prebufCapacity() const { return PREBUF_SAMPLES; }
    // Min pre-buffer fill since the last call, then resets. Better
    // headroom indicator than `lastWaitMicros` — captures the worst
    // moment in the window, not a single snapshot.
    size_t   prebufMinAndReset() {
        size_t m = _prebuf_min_count;
        _prebuf_min_count = _prebuf_count;
        return m;
    }

    void resetFormatLog() { _log_pending = true; _samples_consumed = 0; }
    uint32_t samplesConsumed() const { return _samples_consumed; }

private:
    // Run the 256-pt FFT on the latest _spec_samples window and add
    // log-grouped magnitudes into the in-progress display column.
    void computeSpectrum() {
        for (int i = 0; i < SPEC_FFT_SIZE; i++) {
            _spec_real[i] = _spec_samples[i];
            _spec_imag[i] = 0.0f;
        }
        _spec_fft->windowing(FFTWindow::Hann, FFTDirection::Forward);
        _spec_fft->compute(FFTDirection::Forward);
        _spec_fft->complexToMagnitude();
        // Log-group bins 1..N/2-1 (skip DC) into SPEC_BINS visible bands.
        const int half = SPEC_FFT_SIZE / 2;
        const float log_lo = logf(1.0f);
        const float log_hi = logf((float)(half - 1));
        const float step   = (log_hi - log_lo) / SPEC_BINS;
        int b = 0;
        float band_max = 0.0f;
        float next_edge = expf(log_lo + step);
        for (int k = 1; k < half && b < SPEC_BINS; k++) {
            // +6 dB/oct tilt: multiply by k so music's natural low-end
            // bias doesn't park the bottom bands at one colour.
            float mag = _spec_real[k] * (float)k;
            if (mag > band_max) band_max = mag;
            if ((float)k >= next_edge) {
                if (band_max > _spec_accum[b]) _spec_accum[b] = band_max;
                b++;
                band_max = 0.0f;
                next_edge = expf(log_lo + step * (b + 1));
            }
        }
        if (b < SPEC_BINS && band_max > _spec_accum[b]) {
            _spec_accum[b] = band_max;
        }
    }

    // Commit the accumulated column into the ring. Magnitudes are mapped
    // to 0..255 via a log curve so a wide dynamic range fits the LUT.
    void commitSpectrumColumn() {
        // 0 dB reference = full-scale single-bin magnitude (= N/2 for an
        // un-windowed sine). Typical music content lands well below this,
        // spreading nicely across the LUT instead of saturating at yellow.
        constexpr float REF = SPEC_FFT_SIZE / 2.0f;
        constexpr float FLOOR_DB = -80.0f;
        uint32_t h = _spec.head;
        for (int b = 0; b < SPEC_BINS; b++) {
            float m  = _spec_accum[b] / REF;
            float db = (m > 1e-8f) ? 20.0f * log10f(m) : -160.0f;
            if (db < FLOOR_DB) db = FLOOR_DB;
            if (db > 0.0f)     db = 0.0f;
            int v = (int)((db - FLOOR_DB) * (255.0f / -FLOOR_DB));
            _spec.intensity[h][b] = (uint8_t)v;
            _spec_accum[b] = 0.0f;
        }
        _spec.head = (h + 1) % SPEC_COLS;
        _spec.abs_head++;
    }

    void copyAndSubmit(size_t n) {
        // Copy n samples from pre-buffer tail into the next _buf slot
        // (contiguous — playRaw needs it that way), then submit.
        for (size_t i = 0; i < n; i++) {
            _buf[_ring][i] = _prebuf[_prebuf_tail];
            _prebuf_tail = (_prebuf_tail + 1) % PREBUF_SAMPLES;
        }
        _prebuf_count -= n;
        // Track worst-case fill since last diag sample.
        if (_prebuf_count < _prebuf_min_count) _prebuf_min_count = _prebuf_count;

        bool was_drained = _submitted_once && !_spk->isPlaying();
        if (was_drained) ++_underruns;

        constexpr int  PLAY_CHANNEL = 0;
        constexpr bool PREEMPT      = false;
        constexpr uint32_t REPEAT   = 1;
        uint32_t t0 = micros();
        _spk->playRaw(_buf[_ring], n, hertz, false, REPEAT, PLAY_CHANNEL, PREEMPT);
        _last_wait_us = was_drained ? 0 : (micros() - t0);
        _submitted_once = true;
        _ring = (_ring + 1) % RING_COUNT;
    }

    // 3 slots × 1536 samples × 2 bytes ≈ 9 KB. Used only as the
    // contiguous staging area for playRaw; round-robin works because
    // by the time we cycle back to a slot, playRaw will have blocked
    // until the speaker is no longer holding it.
    static constexpr size_t BUF_SIZE   = 1536;
    static constexpr size_t RING_COUNT = 3;

    // Pre-buffer: ~158 ms at 44.1 kHz mono = 7000 samples. Sized a bit
    // larger than the minimum-safe (~150 ms) so a full-slot ship doesn't
    // immediately empty it.
    static constexpr size_t PREBUF_SAMPLES = 7000;  // ~158 ms at 44.1 kHz

    m5::Speaker_Class* _spk = nullptr;
    int16_t  _buf[RING_COUNT][BUF_SIZE]{};
    size_t   _ring = 0;
    int16_t  _prebuf[PREBUF_SAMPLES]{};
    size_t   _prebuf_head  = 0;   // next write
    size_t   _prebuf_tail  = 0;   // next read
    size_t   _prebuf_count = 0;
    size_t   _prebuf_min_count = PREBUF_SAMPLES;  // tracks worst-case fill
    uint32_t _underruns = 0;
    uint32_t _last_wait_us = 0;
    bool     _submitted_once = false;
    bool     _log_pending = false;
    WaveformRing _wv {};
    int16_t  _wv_min = INT16_MAX;
    int16_t  _wv_max = INT16_MIN;
    int      _wv_count = 0;
    uint32_t _samples_consumed = 0;  // reset on resetFormatLog()

    // Spectrum tap state.
    SpectrumRing _spec {};
    bool     _spec_inited     = false;
    float    _spec_samples[SPEC_FFT_SIZE]     = {};
    float    _spec_real[SPEC_FFT_SIZE]        = {};
    float    _spec_imag[SPEC_FFT_SIZE]        = {};
    float    _spec_accum[SPEC_BINS]           = {};
    int      _spec_fill              = 0;
    int      _spec_col_samples       = 0;
    uint32_t _viz_suppress_samples   = 0;
    ArduinoFFT<float>* _spec_fft = nullptr;
};
