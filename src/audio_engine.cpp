#include "audio_engine.h"
#include <cstdio>

namespace scc {

static bool g_paInitialized = false;
static int g_paInitCount = 0;

AudioEngine::AudioEngine(SynthEngine &synth, int sampleRate, int framesPerBuffer)
    : synth_(synth), sampleRate_(sampleRate), framesPerBuffer_(framesPerBuffer) {
    if (!g_paInitialized) {
        PaError err = Pa_Initialize();
        if (err == paNoError) g_paInitialized = true;
        else fprintf(stderr, "sccbox: Pa_Initialize failed: %s\n", Pa_GetErrorText(err));
    }
    g_paInitCount++;
}

AudioEngine::~AudioEngine() {
    stop();
    g_paInitCount--;
    if (g_paInitCount == 0 && g_paInitialized) {
        Pa_Terminate();
        g_paInitialized = false;
    }
}

int AudioEngine::paCallback(const void *, void *output, unsigned long frameCount,
                             const PaStreamCallbackTimeInfo *, PaStreamCallbackFlags,
                             void *userData) {
    auto *self = static_cast<AudioEngine *>(userData);
    self->synth_.render(static_cast<float *>(output), (int)frameCount);
    return paContinue;
}

bool AudioEngine::start(std::string &error) {
    if (running_) return true;
    if (!g_paInitialized) { error = "PortAudio failed to initialize"; return false; }

    PaStreamParameters outParams;
    outParams.device = Pa_GetDefaultOutputDevice();
    if (outParams.device == paNoDevice) {
        error = "no default audio output device found";
        return false;
    }
    outParams.channelCount = 2;
    outParams.sampleFormat = paFloat32;
    outParams.suggestedLatency = Pa_GetDeviceInfo(outParams.device)->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&stream_, nullptr, &outParams, sampleRate_,
                                 framesPerBuffer_, paClipOff, &AudioEngine::paCallback, this);
    if (err != paNoError) {
        error = std::string("Pa_OpenStream failed: ") + Pa_GetErrorText(err);
        stream_ = nullptr;
        return false;
    }

    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        error = std::string("Pa_StartStream failed: ") + Pa_GetErrorText(err);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        return false;
    }

    running_ = true;
    return true;
}

void AudioEngine::stop() {
    if (!running_) return;
    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    running_ = false;
}

} // namespace scc
