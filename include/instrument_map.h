#pragma once
#include "common.h"
#include <cstdint>

namespace scc {

// The 8 internal SCC-style timbres. Index order is stable and referenced
// by TimbreId below.
enum TimbreId : int {
    TIMBRE_LEAD_SQUARE = 0,
    TIMBRE_BASS_TRIANGLE = 1,
    TIMBRE_BELL_SINE = 2,
    TIMBRE_PLUCK = 3,
    TIMBRE_PAD_TRIANGLE = 4,
    TIMBRE_NOISE_PERC = 5,
    TIMBRE_ORGAN_SQUARE = 6,
    TIMBRE_SINE_LEAD = 7,
    TIMBRE_COUNT = 8
};

const Timbre &timbre_by_id(int id);

// Maps a General MIDI program number (0-127) to one of the 8 timbres.
int gm_program_to_timbre(uint8_t program);

// MIDI channel 10 (index 9, zero-based) is the GM percussion channel by
// convention regardless of program number.
inline bool is_percussion_channel(uint8_t channel0based) { return channel0based == 9; }

} // namespace scc
