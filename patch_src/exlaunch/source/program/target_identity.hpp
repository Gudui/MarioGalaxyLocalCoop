#pragma once

#include "lib.hpp"
#include "program/galaxy_config.hpp"

namespace galaxy::target {

using Game = galaxycoop::config::Game;

struct Identity {
    Game game;
    const char* executableSha256;
    bool valid;
};

Identity DetectAndValidate();
const char* GameName(Game game);

}  // namespace galaxy::target
