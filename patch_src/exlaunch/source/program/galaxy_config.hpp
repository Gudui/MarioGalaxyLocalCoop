#pragma once

namespace galaxycoop::config {

// Set by one confirmed target adapter at build time. The bootstrap intentionally
// carries no offsets and installs no behavior-changing hooks.
enum class Game { Unknown, Smg1, Smg2 };

struct BuildIdentity {
    Game game;
    const char* executableSha256;
};

const BuildIdentity& GetBuildIdentity();

}  // namespace galaxycoop::config
