#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace scc {

enum class MidiEventType : uint8_t { NoteOn, NoteOff, ProgramChange };

struct MidiEvent {
    uint32_t tick;
    double   timeSeconds; // resolved via tempo map after parsing
    MidiEventType type;
    uint8_t  channel;  // 0-15
    uint8_t  data1;    // note number or program number
    uint8_t  data2;    // velocity (NoteOn/NoteOff only)
};

struct MidiFile {
    uint16_t format = 1;
    uint16_t division = 480; // ticks per quarter note
    std::vector<MidiEvent> events; // sorted by tick, merged across tracks
    double durationSeconds = 0.0;
};

// Parses a Standard MIDI File (.mid) from disk. Supports format 0/1,
// running status, and standard meta/channel events. SMPTE-based division
// is not supported (rare in practice) and will produce an error.
bool parse_midi_file(const std::string &path, MidiFile &out, std::string &error);

} // namespace scc
