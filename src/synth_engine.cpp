#include "synth_engine.h"
#include "instrument_map.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace scc {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double note_to_freq(int note) {
    return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

// Standard polyBLEP (polynomial band-limited step) correction, used to
// smooth the discontinuities in a naive square wave so it doesn't alias
// as harshly as an idealized mathematical square. t is the oscillator's
// phase position (0..1) relative to the discontinuity being corrected;
// dt is the phase increment per sample (i.e. frequency/sampleRate).
static inline double poly_blep(double t, double dt) {
    if (dt <= 0.0) return 0.0;
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    } else if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

// Deterministic (not random-per-run) small per-channel detune, in cents,
// giving each of the 16 MIDI channels a fixed, consistent "slightly off"
// pitch color -- reminiscent of multi-oscillator vintage hardware where
// no two channels are perfectly in tune with each other. Kept small
// (<= ~4 cents) and deterministic so behavior is reproducible run to run.
static float channel_detune_cents(int channel) {
    channel &= 15;
    return 4.0f * std::sin((float)channel * 2.399963f); // irrational-ish step avoids periodic patterns
}

// Deterministic small per-channel amplitude bias simulating an imperfect
// analog mixing bus, where channels don't sum with perfectly equal gain.
// Stays within roughly [0.85, 1.15].
static float channel_amplitude_bias(int channel) {
    channel &= 15;
    return 1.0f + 0.15f * std::sin((float)channel * 1.7f + 0.5f);
}

// Exponential decay needs ~ln(1000) = 6.908 time constants to reach 0.1%
// (~-60dB) of its starting value. Using this scaling factor when
// converting a nominal ADSR time (seconds) into a per-sample
// multiplicative coefficient means a given adsr.release/adsr.decay value
// reaches "effectively silent" within approximately that many seconds,
// matching what the previous linear-ramp implementation guaranteed
// exactly. Without this factor, a coefficient of exp(-1/(T*sr)) only
// reaches ~37% of its starting value after T seconds and takes roughly
// 7x longer than T to actually become inaudible.
static constexpr float kEnvDecayLn = 6.907755f; // ln(1000)

static float exp_coeff_for_time(float seconds, int sampleRate) {
    float tc = std::max(seconds, 0.0005f);
    return std::exp(-kEnvDecayLn / (tc * (float)sampleRate));
}

SynthEngine::SynthEngine(int sampleRate, int maxVoices)
    : sampleRate_(sampleRate), maxVoices_(std::clamp(maxVoices, 1, 32)) {
    voices_.resize(maxVoices_);
    fadeVoices_.resize(8);
    muted_.assign(maxVoices_, false);
    recomputeMasterLowpassCoeff();
}

void SynthEngine::recomputeMasterLowpassCoeff() {
    double cutoff = std::clamp((double)masterLpCutoffHz_, 20.0, (double)sampleRate_ * 0.49);
    double coeff = 1.0 - std::exp(-2.0 * M_PI * cutoff / sampleRate_);
    masterLpCoeff_ = (float)std::clamp(coeff, 0.001, 0.999);
}

void SynthEngine::setPhaseDriftEnabled(bool enabled) {
    std::lock_guard<std::mutex> lk(mutex_);
    phaseDriftEnabled_ = enabled;
}

void SynthEngine::setMasterLowpassEnabled(bool enabled) {
    std::lock_guard<std::mutex> lk(mutex_);
    masterLpEnabled_ = enabled;
}

void SynthEngine::setMasterLowpassCutoffHz(float hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    masterLpCutoffHz_ = std::clamp(hz, 2000.0f, 18000.0f);
    recomputeMasterLowpassCoeff();
}

void SynthEngine::setMaxVoices(int n) {
    n = std::clamp(n, 1, 32);
    std::lock_guard<std::mutex> lk(mutex_);
    if (n < (int)voices_.size()) {
        // Fade out any voices about to be dropped so shrinking doesn't click.
        for (int i = n; i < (int)voices_.size(); i++) {
            if (voices_[i].stage != Voice::Stage::Idle) startFadeSteal(i);
        }
    }
    voices_.resize(n);
    muted_.resize(n, false); // preserves existing mute states; new slots start unmuted
    maxVoices_ = n;
}

int SynthEngine::maxVoices() const { return maxVoices_; }

void SynthEngine::setVoiceMuted(int index, bool muted) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || index >= (int)muted_.size()) return;
    muted_[index] = muted;
}

bool SynthEngine::isVoiceMuted(int index) const {
    // Single bool read; not locking here matches the existing precedent
    // of stylePreset() also reading a simple field without the mutex.
    if (index < 0 || index >= (int)muted_.size()) return false;
    return muted_[index];
}

void SynthEngine::setStylePreset(StylePreset p) {
    std::lock_guard<std::mutex> lk(mutex_);
    style_ = p;
}

Waveform SynthEngine::effectiveWaveform(Waveform w) const {
    switch (style_) {
        case StylePreset::PureSquare:   return Waveform::Square;
        case StylePreset::PureTriangle: return Waveform::Triangle;
        case StylePreset::BrightSine:   return Waveform::Sine;
        default: return w;
    }
}

int SynthEngine::findFreeVoice() {
    for (int i = 0; i < (int)voices_.size(); i++)
        if (voices_[i].stage == Voice::Stage::Idle) return i;
    return -1;
}

int SynthEngine::chooseVoiceToSteal() {
    // Oldest-note-cutoff: steal whichever currently-sounding voice was
    // allocated longest ago (smallest `age`), full stop. This replaces
    // the previous priority-tiered strategy; Priority is still tracked
    // and displayed (see VoiceSnapshot/priority_css_class in the UI) but
    // no longer influences which voice gets stolen.
    int best = -1;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (int i = 0; i < (int)voices_.size(); i++) {
        if (voices_[i].stage == Voice::Stage::Idle) continue; // shouldn't happen (caller already checked), but be safe
        if (voices_[i].age < oldest) {
            oldest = voices_[i].age;
            best = i;
        }
    }
    return best;
}

void SynthEngine::startFadeSteal(int idx) {
    // Move the voice's current state into the overflow fade pool so it can
    // ring out over a few milliseconds instead of clicking off instantly.
    Voice &src = voices_[idx];
    if (src.stage == Voice::Stage::Idle) return;

    int slot = -1;
    for (int i = 0; i < (int)fadeVoices_.size(); i++)
        if (fadeVoices_[i].stage == Voice::Stage::Idle) { slot = i; break; }
    bool poolSlotWasIdle = (slot >= 0);
    if (slot < 0) slot = 0; // pool exhausted: reuse oldest, tiny risk of a seam

    Voice v = src;
    v.stage = Voice::Stage::Stolen;
    const float fadeSeconds = 0.006f;
    // Exponential coefficient targeting silence over fadeSeconds, matching
    // the same multiplicative-decay approach used for normal Release.
    v.releaseRate = exp_coeff_for_time(fadeSeconds, sampleRate_);
    fadeVoices_[slot] = v;
    // Only count this as a newly-active voice if the pool slot we used was
    // previously idle; if the pool was exhausted and we reused an
    // already-active slot, it was already counted.
    if (poolSlotWasIdle) activeCount_++;

    // src is unconditionally going non-idle -> idle here. Callers that
    // immediately repurpose this same slot for a new note (noteOn) are
    // responsible for incrementing activeCount_ again themselves; callers
    // that are permanently dropping this slot (setMaxVoices shrinking)
    // correctly leave the count decremented.
    src.stage = Voice::Stage::Idle;
    src.active = false;
    src.envLevel = 0.0f;
    activeCount_ = std::max(0, activeCount_ - 1);
}

void SynthEngine::noteOn(int channel, int note, float velocity01, const Timbre &timbre) {
    std::lock_guard<std::mutex> lk(mutex_);
    Priority prio = timbre.defaultPriority;
    if (is_percussion_channel((uint8_t)channel)) prio = Priority::Percussion;

    int idx = findFreeVoice();
    if (idx < 0) idx = chooseVoiceToSteal();
    if (idx < 0) return; // no voices configured at all

    // startFadeSteal() unconditionally decrements activeCount_ for this
    // slot as it goes idle; we unconditionally re-increment below once the
    // slot is definitely non-idle again, so the two nets to +1 overall for
    // this call, whichever path (free slot vs. steal) was taken.
    if (voices_[idx].stage != Voice::Stage::Idle) startFadeSteal(idx);

    Voice &v = voices_[idx];
    v = Voice{}; // reset to defaults
    v.active = true;
    v.note = note;
    v.channel = channel;
    v.priority = prio;
    v.phase = 0.0;
    double noteFreq = note_to_freq(note);

    // Retro renderer bias: a fixed per-channel detune (consistent for
    // that channel every time) plus, if enabled, a small per-voice random
    // drift (<0.5%) so repeated notes on the same channel aren't
    // perfectly identical either -- both stacked multiplicatively onto
    // the base frequency, matching how a real multi-oscillator vintage
    // instrument is never perfectly in tune with itself.
    double freqMultiplier = std::pow(2.0, channel_detune_cents(channel) / 1200.0);
    if (phaseDriftEnabled_) {
        std::uniform_real_distribution<double> driftDist(-0.005, 0.005); // <0.5%
        freqMultiplier *= (1.0 + driftDist(rng_));
    }
    v.phaseInc = (noteFreq * freqMultiplier) / sampleRate_;

    // Gamma velocity curve (gamma=0.8): raw GM velocity (0..127) mapped
    // linearly to 0..1 made mezzo velocities (64-100) read quieter than
    // intended; the <1 exponent lifts the mid-range without changing the
    // extremes (0 stays 0, 1 stays 1).
    v.velocity = std::pow(std::clamp(velocity01, 0.0f, 1.0f), 0.8f);
    // Retro renderer bias: per-channel amplitude scaling simulating an
    // imperfect mixing bus (channels don't sum with perfectly equal
    // gain on real vintage hardware). Allowed to mildly overshoot 1.0;
    // the final tanh soft-clip absorbs any excess gracefully.
    v.velocity = std::clamp(v.velocity * channel_amplitude_bias(channel), 0.0f, 1.3f);

    v.waveform = effectiveWaveform(timbre.waveform);
    v.adsr = timbre.adsr;
    // Independent per-voice attack/release timing jitter (+/-2-5ms) so
    // simultaneous same-timbre notes don't all snap through their
    // envelope stages in perfect lockstep.
    {
        std::uniform_real_distribution<float> magDist(0.002f, 0.005f);
        std::uniform_int_distribution<int> signDist(0, 1);
        auto jitterSeconds = [&]() {
            float mag = magDist(rng_);
            return (signDist(rng_) == 0) ? -mag : mag;
        };
        v.adsr.attack = std::max(0.0005f, v.adsr.attack + jitterSeconds());
        v.adsr.release = std::max(0.001f, v.adsr.release + jitterSeconds());
    }
    v.harmonicColor = timbre.harmonicColor;

    // Filter cutoff relative to note pitch rather than one fixed Hz value
    // per timbre: convert the existing per-timbre coefficient into an
    // equivalent reference cutoff frequency at A4 (440Hz), then scale that
    // reference proportionally to how far this note is from A4. This keeps
    // brightness at A4 identical to the previous fixed-coefficient
    // behavior, while keeping a timbre's harmonic character proportionally
    // consistent across its pitch range instead of high notes losing
    // relatively more top end than low notes did. Not applied to Noise --
    // GM percussion "pitch" doesn't correspond to a meaningful brightness
    // scale, so noise keeps its designed absolute brightness.
    {
        double coeffStatic = std::clamp((double)timbre.filterCutoff, 0.001, 0.999);
        double refHz = -((double)sampleRate_ / (2.0 * M_PI)) * std::log(1.0 - coeffStatic);
        double cutoffHz = refHz;
        if (v.waveform != Waveform::Noise) {
            cutoffHz *= std::clamp(noteFreq / 440.0, 0.5, 3.0);
        }
        double coeff = 1.0 - std::exp(-2.0 * M_PI * cutoffHz / sampleRate_);
        // Ceiling capped a little below fully-open (0.999) as a cheap,
        // always-on stand-in for a lossy DAC's imperfect reconstruction
        // filter -- every voice loses a touch of its top end, not just
        // the ones whose per-timbre coefficient was already dark.
        v.lpCoeff = (float)std::clamp(coeff, 0.01, 0.90);
    }

    v.lfsr = 0xACE1u ^ (uint32_t)(note * 2654435761u);
    v.stage = Voice::Stage::Attack;
    v.envLevel = 0.0f;
    v.age = ++clock_;
    activeCount_++;
}

void SynthEngine::noteOff(int channel, int note) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto &v : voices_) {
        if (v.channel == channel && v.note == note &&
            (v.stage == Voice::Stage::Attack || v.stage == Voice::Stage::Decay ||
             v.stage == Voice::Stage::Sustain)) {
            float rel = std::max(v.adsr.release, 0.001f);
            v.releaseRate = exp_coeff_for_time(rel, sampleRate_);
            v.stage = Voice::Stage::Release;
        }
    }
}

void SynthEngine::allNotesOff() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto &v : voices_) {
        if (v.stage != Voice::Stage::Idle) {
            float rel = std::max(v.adsr.release, 0.001f);
            v.releaseRate = exp_coeff_for_time(rel, sampleRate_);
            v.stage = Voice::Stage::Release;
        }
    }
}

int SynthEngine::activeVoiceCount() {
    std::lock_guard<std::mutex> lk(mutex_);
    int n = 0;
    for (auto &v : voices_) if (v.stage != Voice::Stage::Idle) n++;
    return n;
}

std::vector<SynthEngine::VoiceSnapshot> SynthEngine::snapshotVoices() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<VoiceSnapshot> out;
    out.reserve(voices_.size());
    for (auto &v : voices_) {
        out.push_back({v.stage != Voice::Stage::Idle, v.note, v.envLevel, v.priority, v.waveform});
    }
    return out;
}

void SynthEngine::advanceEnvelope(Voice &v) {
    switch (v.stage) {
        case Voice::Stage::Idle:
            v.envLevel = 0.0f;
            break;
        case Voice::Stage::Attack: {
            float rate = 1.0f / (std::max(v.adsr.attack, 0.0005f) * sampleRate_);
            v.envLevel += rate;
            if (v.envLevel >= 1.0f) {
                v.envLevel = 1.0f;
                v.stage = Voice::Stage::Decay;
                // Precompute the exponential approach-to-sustain coefficient
                // once per stage entry rather than every sample.
                float tc = std::max(v.adsr.decay, 0.0005f);
                v.releaseRate = exp_coeff_for_time(tc, sampleRate_);
            }
            break;
        }
        case Voice::Stage::Decay: {
            // Exponential approach toward the sustain level instead of a
            // linear ramp -- linear decay spends disproportionately long
            // "sounding loud" in dB terms, then falls off a cliff near the
            // end; exponential decay is what most chip/analog envelopes
            // actually do and reads as smoother, more natural.
            v.envLevel = v.adsr.sustain + (v.envLevel - v.adsr.sustain) * v.releaseRate;
            if (std::fabs(v.envLevel - v.adsr.sustain) < 0.001f) {
                v.envLevel = v.adsr.sustain;
                v.stage = Voice::Stage::Sustain;
            }
            break;
        }
        case Voice::Stage::Sustain:
            // held constant until an explicit noteOff moves us to Release
            break;
        case Voice::Stage::Release:
        case Voice::Stage::Stolen:
            // Same exponential-decay rationale as the Decay stage above,
            // this time decaying toward silence rather than toward a
            // sustain level. v.releaseRate here is a multiplicative
            // per-sample coefficient (computed at noteOff/allNotesOff/
            // startFadeSteal), not the old linear per-sample subtraction.
            v.envLevel *= v.releaseRate;
            if (v.envLevel <= 0.001f) {
                v.envLevel = 0.0f;
                v.stage = Voice::Stage::Idle;
                v.active = false;
                activeCount_ = std::max(0, activeCount_ - 1);
            }
            break;
    }
}

float SynthEngine::renderVoiceSample(Voice &v) {
    float raw = 0.0f;
    switch (v.waveform) {
        case Waveform::Square: {
            // Band-limited (polyBLEP) square instead of an idealized
            // mathematical square: the naive (v.phase<0.5)?1:-1 step has
            // infinitely many harmonics and aliases harshly once any of
            // them exceed Nyquist. polyBLEP subtracts a small correction
            // polynomial right at each discontinuity (the rising edge at
            // t=0 and the falling edge at t=0.5) to band-limit it cheaply.
            double t = v.phase;
            double dt = v.phaseInc;
            double value = (t < 0.5) ? 1.0 : -1.0;
            value += poly_blep(t, dt);
            double tFalling = t + 0.5;
            if (tFalling >= 1.0) tFalling -= 1.0;
            value -= poly_blep(tFalling, dt);
            raw = (float)value;
            break;
        }
        case Waveform::Triangle: {
            double t = v.phase;
            raw = (float)(1.0 - 4.0 * std::fabs(std::round(t - 0.25) - (t - 0.25)));
            break;
        }
        case Waveform::Sine: {
            double base = std::sin(2.0 * M_PI * v.phase);
            if (v.harmonicColor > 0.0f) {
                // Ratios measured directly against an isolated matching
                // GXSCC reference note (see instrument_map.cpp / Pluck):
                // H2=.095 H3=.035 H4=.017 H5=.011 relative to the
                // fundamental. Renormalized so harmonicColor=1.0 doesn't
                // push peak amplitude above the plain-sine case by more
                // than the sum of the added partials.
                double blend = 0.095 * std::sin(4.0 * M_PI * v.phase) +
                               0.035 * std::sin(6.0 * M_PI * v.phase) +
                               0.017 * std::sin(8.0 * M_PI * v.phase) +
                               0.011 * std::sin(10.0 * M_PI * v.phase);
                base = (base + (double)v.harmonicColor * blend) / (1.0 + (double)v.harmonicColor * 0.158);
            }
            raw = (float)base;
            break;
        }
        case Waveform::Noise:
        default: {
            // Clock the LFSR every sample, independent of note pitch. The
            // previous implementation only shifted this once per period of
            // note_to_freq(note), which for typical GM percussion notes
            // (35-81, ~65-830Hz) shifts only tens-to-low-hundreds of times
            // per second -- far too slow to read as noise; measured output
            // was a near-fixed-polarity click with ~1-2 sign changes across
            // an entire 100ms hit. Clocking every sample gives genuine
            // broadband noise texture (period of a 15-bit LFSR at 44.1kHz
            // is ~743ms, long enough that no audible pitch/periodicity
            // shows up within any percussive envelope).
            raw = (v.lfsr & 1u) ? 1.0f : -1.0f;
            uint32_t bit = (v.lfsr ^ (v.lfsr >> 1)) & 1u;
            v.lfsr = (v.lfsr >> 1) | (bit << 14);
            return raw; // noise has no "phase" concept; nothing else to advance
        }
    }

    v.phase += v.phaseInc;
    if (v.phase >= 1.0) v.phase -= 1.0;
    return raw;
}

void SynthEngine::render(float *out, int nframes) {
    std::lock_guard<std::mutex> lk(mutex_);
    // Normalize against how many voices are *actually* sounding right now
    // (activeCount_), not the configured polyphony cap (voices_.size()).
    // The old code divided by sqrt(maxVoices) unconditionally, so sparse
    // passages (measured: this engine sits at <=2 active voices ~5% of
    // the time on a typical dense orchestral MIDI file, averaging ~10.4
    // active out of a 16 cap) were attenuated as if the cap were always
    // fully populated, burying quiet sections and flattening dynamic
    // contrast between sparse and dense passages. Read once per block;
    // activeCount_ changing slightly mid-block as voices finish releasing
    // is an acceptable amount of staleness.
    const float norm = 0.36f / std::sqrt((float)std::max(1, activeCount_));

    for (int f = 0; f < nframes; f++) {
        float mix = 0.0f;

        for (int i = 0; i < (int)voices_.size(); i++) {
            Voice &v = voices_[i];
            if (v.stage == Voice::Stage::Idle) continue;
            advanceEnvelope(v);
            float raw = renderVoiceSample(v);
            float scaled = raw * v.envLevel * v.velocity;
            v.lpState += v.lpCoeff * (scaled - v.lpState);
            // Muting only gates this slot's contribution to the mix; its
            // envelope/phase/filter state keeps advancing normally so
            // unmuting mid-note resumes in sync rather than clicking in.
            if (i < (int)muted_.size() && muted_[i]) continue;
            mix += v.lpState;
        }
        for (auto &v : fadeVoices_) {
            if (v.stage == Voice::Stage::Idle) continue;
            advanceEnvelope(v);
            float raw = renderVoiceSample(v);
            float scaled = raw * v.envLevel * v.velocity;
            v.lpState += v.lpCoeff * (scaled - v.lpState);
            mix += v.lpState;
        }

        mix *= norm;
        float clipped = std::tanh(mix); // gentle global soft-clip / mix filter

        if (masterLpEnabled_) {
            // Two cascaded one-pole stages for a steeper "output stage"
            // rolloff than a single pole gives, applied after saturation
            // (sum -> saturate -> lowpass), simulating both the requested
            // optional post-mix filter and a lossy DAC's reconstruction
            // filter in one place rather than as two redundant stages.
            masterLpState1_ += masterLpCoeff_ * (clipped - masterLpState1_);
            masterLpState2_ += masterLpCoeff_ * (masterLpState1_ - masterLpState2_);
            clipped = masterLpState2_;
        }

        out[f * 2 + 0] = clipped;
        out[f * 2 + 1] = clipped;
    }
}

} // namespace scc
