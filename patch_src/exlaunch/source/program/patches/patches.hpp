#pragma once

#include "program/target_identity.hpp"
#include "patch_shared.hpp"
#include "patch_spawning.hpp"
#include "patch_input.hpp"
#include "patch_collision.hpp"
#include "patch_actions.hpp"
#include "patch_warp.hpp"
#include "patch_camera.hpp"
#include "patch_hud.hpp"
#include "patch_lives.hpp"
#include "patch_powerup.hpp"
#include "patch_diagnostics.hpp"
#include "patch_respawn.hpp"
#include "patch_splitscreen.hpp"
#include "patch_effects.hpp"

namespace mod::patches {

inline void InstallAll(const galaxy::target::Identity& identity) {
    InstallSpawning(identity);     // 1. Spawning & Actor capture first
    InstallCollision(identity);    // 2. Collision filtering
    InstallInput(identity);        // 3. Controller & Pad routing
    InstallActions(identity);      // 4. Spin attack synchronization
    InstallWarp(identity);         // 5. Warp recovery & Bubble flight
    InstallCamera(identity);       // 6. Frustum & Midpoint camera
    InstallHud(identity);          // 7. Dual Health Meters & UI
    InstallLives(identity);        // 8. Independent Lives & 1-Up Routing
    InstallPowerUp(identity);      // 9. PATCH-0172 shared power-ups
    InstallRespawn(identity);
    InstallSplitScreen(identity);  // DIAG-0173 fixed split (default off)
    InstallEffects(identity);      // ATT-0214 emitter pool guard
    InstallDiagnostics(identity); // 9. Diagnostics (if enabled)
}

}  // namespace mod::patches
