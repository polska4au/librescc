#include "player.h"
#include "instrument_map.h"
#include <chrono>
#include <algorithm>
#include <filesystem>

namespace scc {

using clock_type = std::chrono::steady_clock;

Player::Player(SynthEngine &synth) : synth_(synth) {}

Player::~Player() {
    stop();
    joinSchedulerIfAny();
}

void Player::joinSchedulerIfAny() {
    if (schedulerThread_.joinable()) schedulerThread_.join();
}

bool Player::loadFile(const std::string &path, std::string &error) {
    stop();
    joinSchedulerIfAny();

    MidiFile parsed;
    if (!parse_midi_file(path, parsed, error)) return false;

    midi_ = std::move(parsed);
    fileName_ = std::filesystem::path(path).filename().string();
    positionSeconds_ = 0.0;
    return true;
}

void Player::play() {
    if (playing_ || midi_.events.empty()) return;
    stopRequested_ = false;
    playing_ = true;
    double startFrom = positionSeconds_.load();
    joinSchedulerIfAny();
    schedulerThread_ = std::thread(&Player::schedulerLoop, this, startFrom);
}

void Player::stop() {
    stopRequested_ = true;
    playing_ = false;
    joinSchedulerIfAny();
    synth_.allNotesOff();
    positionSeconds_ = 0.0;
}

void Player::schedulerLoop(double startSeconds) {
    // Track current instrument per MIDI channel so NoteOn events can pick
    // the right SCC timbre without re-scanning prior ProgramChange events.
    int channelProgram[16];
    std::fill(std::begin(channelProgram), std::end(channelProgram), 0);

    auto wallStart = clock_type::now() - std::chrono::duration_cast<clock_type::duration>(
                                              std::chrono::duration<double>(startSeconds));

    for (auto &ev : midi_.events) {
        if (stopRequested_) break;
        if (ev.timeSeconds < startSeconds) {
            // Fast-forward: still apply program changes so state is correct,
            // but don't sleep or trigger notes for events already in the past.
            if (ev.type == MidiEventType::ProgramChange) channelProgram[ev.channel] = ev.data1;
            continue;
        }

        auto target = wallStart + std::chrono::duration_cast<clock_type::duration>(
                                       std::chrono::duration<double>(ev.timeSeconds));
        while (!stopRequested_ && clock_type::now() < target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            positionSeconds_ = std::chrono::duration<double>(clock_type::now() - wallStart).count();
        }
        if (stopRequested_) break;

        positionSeconds_ = ev.timeSeconds;

        switch (ev.type) {
            case MidiEventType::ProgramChange:
                channelProgram[ev.channel] = ev.data1;
                break;
            case MidiEventType::NoteOn: {
                int timbreId = is_percussion_channel(ev.channel)
                                   ? TIMBRE_NOISE_PERC
                                   : gm_program_to_timbre((uint8_t)channelProgram[ev.channel]);
                synth_.noteOn(ev.channel, ev.data1, ev.data2 / 127.0f, timbre_by_id(timbreId));
                break;
            }
            case MidiEventType::NoteOff:
                synth_.noteOff(ev.channel, ev.data1);
                break;
        }
    }

    if (!stopRequested_) {
        // Reached end of file naturally.
        positionSeconds_ = midi_.durationSeconds;
        synth_.allNotesOff();
    }
    playing_ = false;
}

} // namespace scc
