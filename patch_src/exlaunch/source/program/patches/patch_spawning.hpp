#pragma once

#include "program/target_identity.hpp"

namespace mod::patches {

void InstallSpawning(const galaxy::target::Identity& identity);
void PumpDeferredP2Spawn();

}  // namespace mod::patches
