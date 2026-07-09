#include "synth_engine.h"
#include "instrument_map.h"
#include <cstdio>
#include <vector>

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); return 1; } } while (0)

int main() {
    scc::SynthEngine synth(44100, 4); 
    for (int i = 0; i < 4; i++)
        synth.noteOn(0, 60 + i, 0.9f, scc::timbre_by_id(scc::TIMBRE_LEAD_SQUARE));
    CHECK(synth.activeVoiceCount() == 4, "4 note-ons into a 4-voice engine should fill all voices");

    std::vector<float> buf(256 * 2);
    synth.render(buf.data(), 256);

    synth.noteOn(0, 72, 0.9f, scc::timbre_by_id(scc::TIMBRE_LEAD_SQUARE));
    CHECK(synth.activeVoiceCount() == 4, "polyphony cap must hold after stealing");

    synth.noteOn(9, 38, 1.0f, scc::timbre_by_id(scc::TIMBRE_NOISE_PERC));
    CHECK(synth.activeVoiceCount() == 4, "polyphony cap must hold after a percussion steal too");

    bool anyNonZero = false;
    bool inRange = true;
    for (int block = 0; block < 20; block++) {
        synth.render(buf.data(), 256);
        for (float s : buf) {
            if (s != 0.0f) anyNonZero = true;
            if (s < -1.0001f || s > 1.0001f) inRange = false;
        }
    }
    CHECK(anyNonZero, "rendered audio should not be silent while voices are active");
    CHECK(inRange, "rendered samples must stay within [-1, 1] (soft-clip working)");

    synth.allNotesOff();
    for (int block = 0; block < 200; block++) synth.render(buf.data(), 256);
    CHECK(synth.activeVoiceCount() == 0, "all voices should settle to idle after allNotesOff + render");
    
    synth.setMaxVoices(2);
    CHECK(synth.maxVoices() == 2, "setMaxVoices should update the reported cap");
    for (int i = 0; i < 5; i++)
        synth.noteOn(0, 40 + i, 0.9f, scc::timbre_by_id(scc::TIMBRE_BASS_TRIANGLE));
    CHECK(synth.activeVoiceCount() <= 2, "polyphony cap must hold after shrinking maxVoices");

    printf("test_synth_engine: OK\n");
    return 0;
}
