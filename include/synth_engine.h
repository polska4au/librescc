#pragma once
#include "common.h"
#include <vector>
#include <mutex>
#include <cstdint>
#include <random>

namespace scc {

struct Voice {
    bool active = false;
    int note = -1;
    int channel = -1;
    Priority priority = Priority::Filler;

    double phase = 0.0;
    double phaseInc = 0.0;
    float velocity = 0.0f;

    enum class Stage { Idle, Attack, Decay, Sustain, Release, Stolen } stage = Stage::Idle;
    float envLevel = 0.0f;
    // Per-sample multiplicative decay coefficient (exponential envelope).
    // Computed fresh at each stage-entry point (Attack->Decay, noteOff/
    // allNotesOff->Release, startFadeSteal->Stolen) since each stage has
    // a different target time constant.
    float releaseRate = 0.0f;

    Waveform waveform = Waveform::Square;
    ADSR adsr;

    // one-pole low-pass filter state, per voice, to soften aliasing
    float lpState = 0.0f;
    float lpCoeff = 0.3f;
    float harmonicColor = 0.0f; // see Timbre::harmonicColor

    uint32_t lfsr = 0xACE1u; // for noise waveform

    uint64_t age = 0; // allocation order, used for stable steal tie-breaks
};

class SynthEngine {
public:
    explicit SynthEngine(int sampleRate, int maxVoices = 16);

    void setMaxVoices(int n);           // 12-16 typical range, clamped to [1,32]
    int  maxVoices() const;
    void setStylePreset(StylePreset p);
    StylePreset stylePreset() const { return style_; }

    // channel is 0-15 (MIDI channel), note is 0-127.
    void noteOn(int channel, int note, float velocity01, const Timbre &timbre);
    void noteOff(int channel, int note);
    void allNotesOff();

    // Renders nframes of interleaved stereo float samples into out
    // (size must be >= nframes*2). Safe to call from the audio thread;
    // internally locked against the control-rate note on/off calls.
    void render(float *out, int nframes);

    // For the UI: how many primary voices are currently sounding.
    int activeVoiceCount();

    // Minimal support for the UI's per-channel mute toggles. This only
    // gates a primary voice slot's contribution to the final mix in
    // render() -- it does not touch note allocation, voice stealing,
    // envelopes, or waveform synthesis, and takes effect on the very next
    // rendered sample (no playback restart needed). index is a voice
    // *slot* index (0..maxVoices()-1), matching the slot ordering exposed
    // by snapshotVoices(), not a MIDI channel number.
    void setVoiceMuted(int index, bool muted);
    bool isVoiceMuted(int index) const;

    // "Retro renderer" character controls.
    //
    // Always on (not exposed as toggles, per spec): band-limited/polyBLEP
    // square oscillator, per-voice attack/release timing jitter
    // (+/-2-5ms), per-channel detune drift and per-channel amplitude
    // bias ("imperfect mixing bus"), and a small fixed per-voice
    // brightness ceiling standing in for lossy DAC harmonic roll-off.
    //
    // Explicitly optional (toggleable) per spec:
    void setPhaseDriftEnabled(bool enabled);
    bool phaseDriftEnabled() const { return phaseDriftEnabled_; }

    void setMasterLowpassEnabled(bool enabled);
    bool masterLowpassEnabled() const { return masterLpEnabled_; }
    void setMasterLowpassCutoffHz(float hz); // clamped to [2000, 18000]; ~8-12kHz is the intended range
    float masterLowpassCutoffHz() const { return masterLpCutoffHz_; }

    struct VoiceSnapshot {
        bool active;
        int note;
        float envLevel;
        Priority priority;
        Waveform waveform;
    };
    std::vector<VoiceSnapshot> snapshotVoices();

private:
    int findFreeVoice();
    int chooseVoiceToSteal(); // oldest-allocated voice wins (LRU-style cutoff)
    void startFadeSteal(int idx);
    float renderVoiceSample(Voice &v);
    void advanceEnvelope(Voice &v);
    Waveform effectiveWaveform(Waveform w) const;
    void recomputeMasterLowpassCoeff();

    int sampleRate_;
    int maxVoices_;
    StylePreset style_ = StylePreset::SccClassic;

    std::vector<Voice> voices_;     // primary, counts toward polyphony limit
    std::vector<Voice> fadeVoices_; // small overflow pool for click-free steals
    std::vector<bool> muted_;       // parallel to voices_; UI-driven output gate
    uint64_t clock_ = 0;
    int activeCount_ = 0; // incrementally maintained count of non-Idle voices
                          // across voices_ + fadeVoices_, used to normalize
                          // render() gain against how many voices are
                          // *actually* sounding rather than the configured
                          // maxVoices_ cap.
    std::mutex mutex_;

    // Retro-character state (see setPhaseDriftEnabled/setMasterLowpass*).
    std::mt19937 rng_{std::random_device{}()};
    bool phaseDriftEnabled_ = true;
    bool masterLpEnabled_ = true;
    float masterLpCutoffHz_ = 10000.0f; // default sits mid-range of the requested ~8-12kHz
    float masterLpCoeff_ = 0.5f;        // recomputed from cutoff; see recomputeMasterLowpassCoeff()
    float masterLpState1_ = 0.0f;       // two cascaded one-pole stages for a steeper, more
    float masterLpState2_ = 0.0f;       // "filtered output stage" rolloff than a single pole gives
};

} // namespace scc
