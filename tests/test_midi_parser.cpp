#include "midi_file.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); return 1; } } while (0)

int main(int argc, char **argv) {
    std::string path = (argc > 1) ? argv[1] : "test-midi/flowr_test.mid";
    scc::MidiFile mf;
    std::string err;
    bool ok = scc::parse_midi_file(path, mf, err);
    CHECK(ok, "parse_midi_file should succeed on the bundled test file");
    CHECK(mf.division == 480, "division should match the generated file's PPQ");
    CHECK(!mf.events.empty(), "parsed event list should not be empty");
    CHECK(mf.durationSeconds > 0.0, "duration should be positive");

    // First event should be the melody's program change at tick 0.
    CHECK(mf.events.front().tick == 0, "first event should be at tick 0");

    // At 128 BPM, a half-note (240 ticks @ division 480) is exactly
    // (60/128)/2 seconds. Find the first NoteOff on channel 0 and check it.
    bool foundNoteOff = false;
    for (auto &ev : mf.events) {
        if (ev.type == scc::MidiEventType::NoteOff && ev.channel == 0) {
            double expected = (60.0 / 128.0) / 2.0;
            CHECK(std::fabs(ev.timeSeconds - expected) < 0.005, "tempo-resolved timing should match expected value");
            foundNoteOff = true;
            break;
        }
    }
    CHECK(foundNoteOff, "expected at least one NoteOff on channel 0");

    // Events must be sorted by tick.
    for (size_t i = 1; i < mf.events.size(); i++) {
        CHECK(mf.events[i].tick >= mf.events[i - 1].tick, "events must be sorted by tick");
    }

    printf("test_midi_parser: OK (%zu events, %.2fs)\n", mf.events.size(), mf.durationSeconds);
    return 0;
}
