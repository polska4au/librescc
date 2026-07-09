#pragma once
#include "midi_file.h"
#include "synth_engine.h"
#include <atomic>
#include <thread>
#include <string>

namespace scc {

class Player {
public:
    explicit Player(SynthEngine &synth);
    ~Player();

    // Loads and parses a MIDI file. Stops any current playback first.
    bool loadFile(const std::string &path, std::string &error);

    void play();  // resumes/starts from current position
    void stop();  // stops and resets position to 0
    bool isPlaying() const { return playing_; }

    const std::string &loadedFileName() const { return fileName_; }
    double durationSeconds() const { return midi_.durationSeconds; }
    double positionSeconds() const { return positionSeconds_; }

private:
    void schedulerLoop(double startSeconds);
    void joinSchedulerIfAny();

    SynthEngine &synth_;
    MidiFile midi_;
    std::string fileName_;

    std::thread schedulerThread_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<double> positionSeconds_{0.0};
};

} // namespace scc
