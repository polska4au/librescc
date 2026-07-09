#pragma once
#include "synth_engine.h"
#include <portaudio.h>
#include <string>

namespace scc {

class AudioEngine {
public:
    AudioEngine(SynthEngine &synth, int sampleRate = 44100, int framesPerBuffer = 256);
    ~AudioEngine();

    // Returns false and fills `error` on failure (e.g. no audio device).
    bool start(std::string &error);
    void stop();
    bool isRunning() const { return running_; }
    int sampleRate() const { return sampleRate_; }

private:
    static int paCallback(const void *input, void *output, unsigned long frameCount,
                           const PaStreamCallbackTimeInfo *timeInfo,
                           PaStreamCallbackFlags statusFlags, void *userData);

    SynthEngine &synth_;
    PaStream *stream_ = nullptr;
    int sampleRate_;
    int framesPerBuffer_;
    bool running_ = false;
};

} // namespace scc
