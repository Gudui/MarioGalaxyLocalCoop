#pragma once

#include "program/target_identity.hpp"
#include <cstdint>

namespace mod::patches {

void InstallWarp(const galaxy::target::Identity& identity);
void TryDevBootWarp();
void LogWarpTopSnapshot(const char* tag);
void NotifyStageSpawnWarp(std::uintptr_t scene);
const float* GetCameraPositionForActor(std::uintptr_t actor, const float* nativePosition);
bool ApplyRecoveryCameraMidpoint(float* watch, float* eye, const float* player1, const float* player2);

}  // namespace mod::patches
