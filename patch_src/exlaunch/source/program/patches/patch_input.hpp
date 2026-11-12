#pragma once

#include "program/target_identity.hpp"
#include <cstdint>

namespace mod::patches {

void InstallInput(const galaxy::target::Identity& identity);
// Raw nn::hid NpadButton bits for one Npad id; 0 if disconnected or single Joy-Con.
std::uint64_t ReadNpadRawButtons(std::uint32_t npadId);
// PATCH-0019: true when the actor's native stick query (0x123654) would pass
// movement, i.e. the game currently accepts player movement input.
bool IsNativeMovementGateOpen(void* actor);
// PATCH-0019: bit n set when Npad n has stick movement or A/B/X/Y/ZL held.
std::uint32_t ActivePlayerInputMask();
// ATT-0215: count of MarioActor movements run for player index 0/1 (wraps).
// Unchanged across frames means the game is not moving that player.
unsigned GetMarioMovementSerial(int index);
// DIAG-0176 read-only per-frame probe of the two native player-holder pads.
void TickDiag0176InputSources();

}  // namespace mod::patches
