#include "player.h"
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); return 1; } } while (0)

int main() {
    scc::SynthEngine synth(44100, 16);
    scc::Player player(synth);
    std::string err;

    CHECK(player.loadFile("test-midi/flowr_test.mid", err), "loading the bundled test MIDI file should succeed");
    CHECK(player.durationSeconds() > 0.0, "loaded file should report a positive duration");

    player.play();
    CHECK(player.isPlaying(), "player should report playing immediately after play()");

    std::vector<float> buf(256 * 2);
    bool sawActiveVoices = false;
    for (int i = 0; i < 60 && player.isPlaying(); i++) {
        synth.render(buf.data(), 256);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (synth.activeVoiceCount() > 0) sawActiveVoices = true;
    }
    CHECK(sawActiveVoices, "playback should trigger at least one active voice");

    player.stop();
    CHECK(!player.isPlaying(), "player should report stopped after stop()");
    for (int i = 0; i < 100; i++) synth.render(buf.data(), 256);
    CHECK(synth.activeVoiceCount() == 0, "voices should settle to idle after stop()");

    printf("test_player: OK\n");
    return 0;
}
