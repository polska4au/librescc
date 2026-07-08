#pragma once
#include <cstdint>

namespace scc {

enum class Waveform : uint8_t { Square = 0, Triangle = 1, Sine = 2, Noise = 3, Count = 4 };

// Voice-stealing priority tiers. Higher value = kept longer when voices
// are scarce. Matches the requirement: melody > harmony > percussion > filler.
enum class Priority : uint8_t { Filler = 0, Percussion = 1, Harmony = 2, Melody = 3 };

struct ADSR {
    float attack  = 0.005f;  // seconds
    float decay   = 0.08f;   // seconds
    float sustain = 0.6f;    // level 0..1
    float release = 0.15f;   // seconds
};

// One internal "SCC-style" timbre: a waveform + envelope + filter shape.
struct Timbre {
    const char *name;
    Waveform waveform;
    ADSR adsr;
    float filterCutoff; // 0..1, normalized one-pole coefficient baseline
    Priority defaultPriority;
    // Optional, Sine-waveform-only enrichment: blends in a small amount of
    // 2nd/3rd/4th/5th harmonic content at fixed ratios measured directly
    // against an isolated matching GXSCC reference note (see
    // instrument_map.cpp / Pluck). 0.0 = pure sine (unchanged default
    // behavior for every other Sine timbre); 1.0 = exactly the measured
    // ratios. Has no effect on non-Sine waveforms.
    float harmonicColor = 0.0f;
};

// Global waveform-style presets selectable from the UI dropdown. These
// bend the whole instrument set toward a particular color without
// discarding the per-timbre envelope/priority design.
enum class StylePreset : uint8_t {
    SccClassic = 0,  // use each timbre's mapped waveform as designed
    PureSquare = 1,  // force every voice to square (classic 2A03/AY feel)
    PureTriangle = 2,// force every voice to triangle (soft bass-y feel)
    BrightSine = 3,  // force every voice to sine, filter opened up
    Count = 4
};

inline const char *style_preset_name(StylePreset p) {
    switch (p) {
        case StylePreset::SccClassic:   return "SCC Classic";
        case StylePreset::PureSquare:   return "Pure Square";
        case StylePreset::PureTriangle: return "Pure Triangle";
        case StylePreset::BrightSine:   return "Bright Sine";
        default: return "?";
    }
}

} // namespace scc
