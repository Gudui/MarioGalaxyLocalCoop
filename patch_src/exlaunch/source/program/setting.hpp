#pragma once

#include "common.hpp"

#define EXL_MODULE_NAME "GalaxyCoop"
#define EXL_DEBUG
#define EXL_USE_FAKEHEAP

namespace exl::setting {
constexpr size_t HeapSize = 0x5000;
// Raised from 0x1000: 21 live trampolines exactly fill HookMax 20, so any
// 22nd hook aborted at boot (EVD-0176). 0x2000 allowed 40; the 41st
// (DIAG-0181, ATT-0132) aborted at boot again. 0x3000 allowed 61; the two
// DIAG-0208 v4 hooks (ATT-0190) overflowed it at boot. 0x4000 allows 81;
// 0x5000 leaves room for the four DIAG-0210 hooks on top of PATCH-0192/0194;
// 0x6000 for the thirteen DIAG-0213 hooks.
constexpr size_t JitSize = 0x6000;
constexpr size_t InlinePoolSize = 0x1000;
constexpr size_t LogBufferSize = 512;

static_assert(ALIGN_UP(JitSize, PAGE_SIZE) == JitSize, "");
static_assert(ALIGN_UP(InlinePoolSize, PAGE_SIZE) == InlinePoolSize, "");
}
