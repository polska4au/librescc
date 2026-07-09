#pragma once
#include "player.h"
#include "synth_engine.h"
#include <string>

namespace scc {

int run_ui(Player &player, SynthEngine &synth, const std::string &initialStatus);

} // namespace scc
