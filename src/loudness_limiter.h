// On-the-fly loudness leveling for playback. A fixed input drive gain lifts
// quiet passages, and a lookahead brickwall limiter catches the resulting
// peaks at a ceiling with a slow release — so an audiobook's whisper-to-shout
// swings even out at a fixed volume without pumping. Processes one mono int16
// sample at a time inside the audio task. Disabled by default: when off,
// samples pass through untouched.
//
// Lock-free by construction: the UI task writes plain scalars (and precomputed
// coefficients) through the setters; the audio task reads them per-sample. A
// torn read of one scalar is harmless and self-corrects on the next sample, so
// the per-sample hot path needs no mutex.
#pragma once

#include <cstdint>
#include <cmath>

class LoudnessLimiter {
public:
    // Parameter ranges as the user reasons about them (dB / seconds).
    static constexpr float DRIVE_DB_MIN  = 0.0f;
    static constexpr float DRIVE_DB_MAX  = 24.0f;
    static constexpr float RELEASE_S_MIN = 0.1f;
    static constexpr float RELEASE_S_MAX = 2.0f;

    LoudnessLimiter() { _drive = dbToLinear(_drive_db); recomputeCoefficients(); }

    void setEnabled(bool on)        { _enabled = on; }
    void setDriveDb(float db)       { _drive_db = db; _drive = dbToLinear(db); }
    void setReleaseSeconds(float s) { _release_s = s; recomputeCoefficients(); }
    // The release/attack times and lookahead are expressed in seconds, so they
    // track the live stream's sample rate; call this when the format changes.
    void setSampleRate(uint32_t hz) {
        _sample_rate = hz ? hz : 44100;
        recomputeCoefficients();
    }

    bool  enabled()        const { return _enabled; }
    float driveDb()        const { return _drive_db; }
    float releaseSeconds() const { return _release_s; }

    // Total gain currently applied to the audio vs the raw decoded sample:
    // the static drive multiplied by the live limiter gain. 1.0 (unity) when
    // off. The visualisation taps this to show how much the sound is being
    // amplified moment to moment.
    float netGain() const { return _enabled ? _drive * _gain : 1.0f; }

    // Per-sample hot path. Returns the leveled sample, or the input unchanged
    // when disabled.
    int16_t process(int16_t mono) {
        if (!_enabled) return mono;

        // Drive into the limiter in float so a peak pushed past full-scale
        // doesn't wrap before the limiter pulls it back.
        float driven = (float)mono * _drive;

        // Emit the sample that entered the delay line `_lookahead` samples ago;
        // the gain reduction triggered by newer (louder) samples has had the
        // lookahead window to ramp in before that peak reaches the output.
        float delayed = _delay[_write];
        _delay[_write] = driven;
        _write = (_write + 1) % _lookahead;

        // Drop toward more reduction quickly (attack spans the lookahead so it
        // lands as the peak arrives); recover toward unity slowly (release).
        float peak   = fabsf(driven);
        float target = (peak > _ceiling) ? _ceiling / peak : 1.0f;
        float coeff  = (target < _gain) ? _attack : _release;
        _gain += (target - _gain) * coeff;

        // Brickwall: clamp catches any residue the smoothed gain didn't.
        float out = delayed * _gain;
        if (out >  _ceiling) out =  _ceiling;
        if (out < -_ceiling) out = -_ceiling;
        return (int16_t)out;
    }

    // Drop in-flight state so a stop / skip / seek doesn't replay stale
    // lookahead samples or carry a stale gain envelope across the cut.
    void reset() {
        for (int i = 0; i < LOOKAHEAD_MAX; i++) _delay[i] = 0.0f;
        _write = 0;
        _gain  = 1.0f;
    }

private:
    static float dbToLinear(float db) { return powf(10.0f, db / 20.0f); }

    // Everything that depends on a time constant or the sample rate: the
    // lookahead length and the one-pole attack/release coefficients.
    void recomputeCoefficients() {
        _ceiling = 32767.0f * dbToLinear(CEILING_DBFS);
        int look = (int)(LOOKAHEAD_MS * 0.001f * _sample_rate + 0.5f);
        if (look < 1)             look = 1;
        if (look > LOOKAHEAD_MAX) look = LOOKAHEAD_MAX;
        _lookahead = look;
        // Attack ramp spans the lookahead window so reduction is fully in place
        // by the time the triggering peak reaches the output.
        _attack  = 1.0f - expf(-1.0f / (float)_lookahead);
        _release = 1.0f - expf(-1.0f / (_release_s * (float)_sample_rate));
    }

    static constexpr float LOOKAHEAD_MS  = 3.0f;
    static constexpr int   LOOKAHEAD_MAX = 256;   // ~5.8 ms at 44.1 kHz
    static constexpr float CEILING_DBFS  = -1.0f; // brickwall ceiling

    bool     _enabled     = false;
    float    _drive_db    = 6.0f;
    float    _drive       = 1.0f;
    float    _release_s   = 0.5f;
    uint32_t _sample_rate = 44100;

    float _ceiling   = 0.0f;
    int   _lookahead = 1;
    float _attack    = 0.0f;
    float _release   = 0.0f;

    float _delay[LOOKAHEAD_MAX] = {};
    int   _write = 0;
    float _gain  = 1.0f;
};
