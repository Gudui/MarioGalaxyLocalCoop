#pragma once

#include "program/target_identity.hpp"

namespace mod::patches {

void InstallDiagnostics(const galaxy::target::Identity& identity);

// DIAG-0144: hook-free guarded report of P1/P2 inner+0x8f1; call from owned
// functions only (stage spawn, fatal path). No-op unless kill switch is on.
void LogDiag0144Flags(const char* context);

// DIAG-0182: per-frame +0xF20/+0xF38/+0xF39 transition log for both players.
void TickDiag0182();

// DIAG-0183: logs the native stage-restart start id (8 bytes) on change.
void TickDiag0183();

}  // namespace mod::patches
