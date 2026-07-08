#include "instrument_map.h"

namespace scc {

static const Timbre kTimbres[TIMBRE_COUNT] = {
    /* name            waveform            adsr{atk,dec,sus,rel}          cutoff  priority */
    { "Lead Square",   Waveform::Square,   {0.004f, 0.06f, 0.70f, 0.10f}, 0.55f, Priority::Melody },
    { "Bass Triangle", Waveform::Triangle, {0.006f, 0.10f, 0.85f, 0.12f}, 0.35f, Priority::Harmony },
    { "Bell Sine",     Waveform::Sine,     {0.002f, 0.35f, 0.25f, 0.30f}, 0.90f, Priority::Filler },
    // Pluck: measured directly against an isolated matching GXSCC note
    // (GM program 0/Piano family, same MIDI note). GXSCC's harmonic
    // falloff (H2=.095 H3=.035 H4=.017 H5=.011) is far too steep and has
    // real even-harmonic content to be a square wave -- our old Square
    // choice measured a textbook H3=.334/H5=.200/H7=.142 (i.e. near-ideal
    // 1/n odd-harmonic square signature), a completely different color.
    // Sine is a much closer match (GXSCC reads as "sine with faint
    // impurities", not "filtered square"). Sustain was also 10x too low:
    // GXSCC's envelope holds a ~36% plateau from ~100ms onward on repeated
    // short notes; ours decayed to ~3.5%, making a held bass ostinato
    // sound like a series of decaying transients instead of solid notes.
    { "Pluck",         Waveform::Sine,     {0.002f, 0.12f, 0.40f, 0.08f}, 0.65f, Priority::Filler, 1.0f },
    { "Pad Triangle",  Waveform::Triangle, {0.080f, 0.20f, 0.75f, 0.35f}, 0.30f, Priority::Harmony },
    { "Noise Perc",    Waveform::Noise,    {0.001f, 0.05f, 0.00f, 0.06f}, 0.80f, Priority::Percussion },
    { "Organ Square",  Waveform::Square,   {0.020f, 0.05f, 0.90f, 0.20f}, 0.45f, Priority::Harmony },
    { "Sine Lead",     Waveform::Sine,     {0.010f, 0.08f, 0.75f, 0.18f}, 0.60f, Priority::Melody },
};

const Timbre &timbre_by_id(int id) {
    if (id < 0 || id >= TIMBRE_COUNT) id = TIMBRE_LEAD_SQUARE;
    return kTimbres[id];
}

int gm_program_to_timbre(uint8_t program) {
    // General MIDI groups programs into 16 families of 8. Map each
    // family onto one of our 8 timbres by rough sonic character.
    int family = program / 8; // 0..15
    switch (family) {
        case 0:  return TIMBRE_PLUCK;         // Piano
        case 1:  return TIMBRE_BELL_SINE;     // Chromatic Percussion
        case 2:  return TIMBRE_ORGAN_SQUARE;  // Organ
        case 3:  return TIMBRE_PLUCK;         // Guitar
        case 4:  return TIMBRE_BASS_TRIANGLE; // Bass
        case 5:  return TIMBRE_PAD_TRIANGLE;  // Strings
        case 6:  return TIMBRE_PAD_TRIANGLE;  // Ensemble
        case 7:  return TIMBRE_LEAD_SQUARE;   // Brass
        case 8:  return TIMBRE_LEAD_SQUARE;   // Reed
        case 9:  return TIMBRE_SINE_LEAD;     // Pipe
        case 10: return TIMBRE_LEAD_SQUARE;   // Synth Lead
        case 11: return TIMBRE_PAD_TRIANGLE;  // Synth Pad
        case 12: return TIMBRE_SINE_LEAD;     // Synth Effects
        case 13: return TIMBRE_PLUCK;         // Ethnic
        case 14: return TIMBRE_NOISE_PERC;    // Percussive
        case 15: return TIMBRE_NOISE_PERC;    // Sound Effects
        default: return TIMBRE_LEAD_SQUARE;
    }
}

} // namespace scc
