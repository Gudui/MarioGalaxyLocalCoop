#pragma once

#include "program/target_identity.hpp"

namespace mod::patches {

void InstallActions(const galaxy::target::Identity& identity);
void SyncSpinEnabled(const char* phase);

void NotifyStageSpawn();
void NotifyCameraSeen(std::uintptr_t cameraMan);
void UpdateP2Parking(void* p1, void* p2);
bool IsP2Parked();

}  // namespace mod::patches
