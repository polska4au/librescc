#pragma once
#include "player.h"
#include "synth_engine.h"
#include <string>

namespace scc {

// Runs the GTK4 window/event loop until the user closes it. Blocking call,
// intended to be invoked from main(). Owns no audio/synth state itself --
// it drives the Player and SynthEngine passed in. Signature is identical
// to the previous SDL2 frontend so main.cpp needs no changes.
int run_ui(Player &player, SynthEngine &synth, const std::string &initialStatus);

} // namespace scc
