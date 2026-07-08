#include "synth_engine.h"
#include "audio_engine.h"
#include "player.h"
#include "ui.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
    const int sampleRate = 44100;
    const int framesPerBuffer = 256;
    const int initialMaxVoices = 16;

    scc::SynthEngine synth(sampleRate, initialMaxVoices);
    scc::AudioEngine audio(synth, sampleRate, framesPerBuffer);

    std::string audioErr;
    bool audioOk = audio.start(audioErr);

    std::string status;
    if (!audioOk) {
        fprintf(stderr, "sccbox: audio output unavailable: %s\n", audioErr.c_str());
        fprintf(stderr, "sccbox: continuing with UI running silently.\n");
        status = "Audio device unavailable - UI running silently";
    } else {
        status = "Ready. Drag a .mid file onto the window to load it.";
    }

    scc::Player player(synth);

    // Optional: allow launching with a MIDI file already loaded,
    // e.g. `sccbox test-midi/flowr_test.mid`.
    if (argc > 1) {
        std::string err;
        if (player.loadFile(argv[1], err)) {
            status = "Loaded: " + player.loadedFileName();
        } else {
            fprintf(stderr, "sccbox: failed to load '%s': %s\n", argv[1], err.c_str());
        }
    }

    int rc = scc::run_ui(player, synth, status);

    player.stop();
    audio.stop();
    return rc;
}
