#pragma once
#include "program/target_identity.hpp"

namespace mod::patches {
// PATCH-0172: a power-up taken by either player is granted to both, because the
// native grant path always resolves MarioHolder+0x18 (P1) regardless of who
// touched the item (EVD-0219).
inline constexpr bool kSharedPowerUpEnabled = true;
void InstallPowerUp(const galaxy::target::Identity& identity);
// PATCH-0008 brackets P2's MarioActor visual init (0x1BDC40) with this, so the
// power-up model builders can load P2's Luigi archives (ATT-0125).
void SetP2VisualInitActive(bool active);
}  // namespace mod::patches
