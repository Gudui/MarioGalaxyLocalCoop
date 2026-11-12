#pragma once

#include "program/target_identity.hpp"
#include <cstdint>

namespace mod::patches {

void InstallHud(const galaxy::target::Identity& identity);
void UpdateHud(std::uintptr_t target, std::uintptr_t p1, std::uintptr_t p2);

}  // namespace mod::patches
