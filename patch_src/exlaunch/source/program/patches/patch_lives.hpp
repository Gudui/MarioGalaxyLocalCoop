#pragma once

#include "program/target_identity.hpp"
#include <cstdint>

namespace mod::patches {

// PATCH-0171 / EVD-0207: P2 at 0 lives stays out until a life is gained;
// 1-Up Mushroom goes to a partner at 0 lives; milestone 1-up gives both +1;
// both at 0 lives uses the native all-down path. P1-out not included.
inline constexpr bool kCoopLifeRulesEnabled = true;

// Player Lives state API
std::uint16_t GetP1Lives();
std::uint16_t GetP2Lives();
void SetP2Lives(std::uint16_t lives);
void DecrementP1LivesOnce();
void DecrementP2LivesOnce();
void NotifyStageSpawnLives();
// DIAG-0173 test only: per-frame P2 controller debug 1-Up Mushroom pickup.
void TickDiag0173OneUpTrigger();

void InstallLives(const galaxy::target::Identity& identity);

}  // namespace mod::patches
