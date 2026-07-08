#include "midi_file.h"
#include <fstream>
#include <algorithm>
#include <cstring>

namespace scc {

namespace {

struct Reader {
    const std::vector<uint8_t> &buf;
    size_t pos = 0;
    explicit Reader(const std::vector<uint8_t> &b) : buf(b) {}

    bool eof() const { return pos >= buf.size(); }
    uint8_t u8() { return pos < buf.size() ? buf[pos++] : 0; }
    uint32_t u16() { uint32_t a = u8(), b = u8(); return (a << 8) | b; }
    uint32_t u32() { uint32_t a = u8(), b = u8(), c = u8(), d = u8(); return (a << 24) | (b << 16) | (c << 8) | d; }

    uint32_t vlq() {
        uint32_t value = 0;
        for (int i = 0; i < 5 && !eof(); i++) {
            uint8_t byte = u8();
            value = (value << 7) | (byte & 0x7F);
            if (!(byte & 0x80)) break;
        }
        return value;
    }

    std::string tag4() {
        std::string s;
        for (int i = 0; i < 4; i++) s += (char)u8();
        return s;
    }
};

struct TempoChange {
    uint32_t tick;
    uint32_t usPerQuarter;
};

} // namespace

bool parse_midi_file(const std::string &path, MidiFile &out, std::string &error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open file: " + path; return false; }

    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 14) { error = "file too small to be a MIDI file"; return false; }

    Reader r(buf);
    if (r.tag4() != "MThd") { error = "missing MThd header"; return false; }
    uint32_t hlen = r.u32();
    if (hlen < 6) { error = "malformed MThd length"; return false; }
    uint16_t format = (uint16_t)r.u16();
    uint16_t ntrks = (uint16_t)r.u16();
    uint16_t division = (uint16_t)r.u16();
    // Skip any extra header bytes beyond the standard 6.
    r.pos += (hlen - 6);

    if (division & 0x8000) {
        error = "SMPTE time division is not supported";
        return false;
    }

    out.format = format;
    out.division = division;

    std::vector<MidiEvent> events;
    std::vector<TempoChange> tempoChanges;
    events.reserve(4096);

    for (int t = 0; t < ntrks; t++) {
        if (r.eof()) break;
        std::string tag = r.tag4();
        uint32_t tlen = r.u32();
        size_t trackEnd = r.pos + tlen;
        if (tag != "MTrk") { r.pos = trackEnd; continue; }

        uint32_t tick = 0;
        uint8_t runningStatus = 0;

        while (r.pos < trackEnd && !r.eof()) {
            uint32_t delta = r.vlq();
            tick += delta;

            uint8_t status = r.u8();
            if (status < 0x80) {
                // Running status: this byte is actually the first data byte.
                r.pos -= 1;
                status = runningStatus;
            } else {
                runningStatus = status;
            }

            uint8_t hi = status & 0xF0;
            uint8_t chan = status & 0x0F;

            if (status == 0xFF) {
                uint8_t metaType = r.u8();
                uint32_t len = r.vlq();
                size_t metaEnd = r.pos + len;
                if (metaType == 0x51 && len >= 3) {
                    uint32_t usPerQuarter = (r.u8() << 16);
                    usPerQuarter |= (r.u8() << 8);
                    usPerQuarter |= r.u8();
                    tempoChanges.push_back({tick, usPerQuarter});
                }
                r.pos = metaEnd; // skip whatever we didn't consume
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t len = r.vlq();
                r.pos += len; // skip sysex
            } else if (hi == 0x80) { // Note Off
                uint8_t note = r.u8(), vel = r.u8();
                events.push_back({tick, 0.0, MidiEventType::NoteOff, chan, note, vel});
            } else if (hi == 0x90) { // Note On (velocity 0 == Note Off)
                uint8_t note = r.u8(), vel = r.u8();
                events.push_back({tick, 0.0,
                                   vel == 0 ? MidiEventType::NoteOff : MidiEventType::NoteOn,
                                   chan, note, vel});
            } else if (hi == 0xA0) { // Poly aftertouch
                r.u8(); r.u8();
            } else if (hi == 0xB0) { // Control change
                r.u8(); r.u8();
            } else if (hi == 0xC0) { // Program change
                uint8_t prog = r.u8();
                events.push_back({tick, 0.0, MidiEventType::ProgramChange, chan, prog, 0});
            } else if (hi == 0xD0) { // Channel aftertouch
                r.u8();
            } else if (hi == 0xE0) { // Pitch bend
                r.u8(); r.u8();
            } else {
                // Unknown/unsupported status; bail out of this track to
                // avoid mis-parsing the rest of the stream.
                break;
            }
        }
        r.pos = trackEnd;
    }

    // Sort tempo changes and events by tick (stable to preserve original order at ties).
    std::stable_sort(tempoChanges.begin(), tempoChanges.end(),
                      [](const TempoChange &a, const TempoChange &b) { return a.tick < b.tick; });
    std::stable_sort(events.begin(), events.end(),
                      [](const MidiEvent &a, const MidiEvent &b) { return a.tick < b.tick; });

    // Resolve absolute time in seconds for every event using the tempo map.
    uint32_t baselineTick = 0;
    double baselineSeconds = 0.0;
    uint32_t currentUsPerQuarter = 500000; // default 120 BPM
    size_t tempoIdx = 0;

    auto ticksToSecondsDelta = [&](uint32_t fromTick, uint32_t toTick) -> double {
        double quarters = double(toTick - fromTick) / double(division);
        return quarters * (double(currentUsPerQuarter) / 1e6);
    };

    for (auto &ev : events) {
        while (tempoIdx < tempoChanges.size() && tempoChanges[tempoIdx].tick <= ev.tick) {
            baselineSeconds += ticksToSecondsDelta(baselineTick, tempoChanges[tempoIdx].tick);
            baselineTick = tempoChanges[tempoIdx].tick;
            currentUsPerQuarter = tempoChanges[tempoIdx].usPerQuarter;
            tempoIdx++;
        }
        ev.timeSeconds = baselineSeconds + ticksToSecondsDelta(baselineTick, ev.tick);
    }

    double duration = 0.0;
    if (!events.empty()) duration = events.back().timeSeconds;
    out.events = std::move(events);
    out.durationSeconds = duration;
    return true;
}

} // namespace scc
