#include "patch_diagnostics.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cstdint>

namespace mod::patches {

namespace {

// DIAG-0144: report the native normal-damage-flag bytes (EVD-0144) without
// installing any hook.
//
// History: a behavior-preserving trampoline at 0x2B5750 boot-faulted twice
// (uint8_t-arg build F6CDB590 and int-arg build A789C75B: identical
// `Unmapped Write64 @ 0x6969696900000000` loop at ~10-19s boot) while the
// hook-less build CE9590C5 booted clean into gameplay. The 0x2B5750 site is
// therefore retired as a hook candidate regardless of its statically clean
// opening. This probe only reads, from functions the mod already owns.
constexpr std::uintptr_t kDiag0144InnerMarioOffset = 0x3C0;
constexpr std::uintptr_t kDiag0144FlagOffset = 0x8F1;

// Compile-time kill switch: set false for clean builds with no diagnostics.
constexpr bool kDiag0144Enabled = false; // fulfilled: EVD-0190/0193

// Guarded read of inner-Mario+0x8f1 for a captured outer actor.
// Returns -1 when any proven pointer hop fails validation.
int ReadInnerEnableFlag(void* actor) {
    if (!IsValidMarioActor(actor)) {
        return -1;
    }
    const std::uintptr_t actorAddr = reinterpret_cast<std::uintptr_t>(actor);
    const std::uintptr_t inner = *reinterpret_cast<const std::uintptr_t*>(actorAddr + kDiag0144InnerMarioOffset);
    if (!IsGuestPointer(inner)) {
        return -1;
    }
    return static_cast<int>(*reinterpret_cast<const std::uint8_t*>(inner + kDiag0144FlagOffset));
}

// DIAG-0145: Read-only trampoline hook on MarioActor::damage (0x25E90C).
// Records damage argument, health transition, velocities, and flags for P1 vs P2.
constexpr std::uintptr_t kDiag0145MarioDamage = 0x25E90C;
// Re-armed as DIAG-0181 (EVD-0232): one Karikari hit on P1 dropped both
// actors' HP. Adds the caller offset so each HP drop names its call site.
constexpr bool kDiag0145Enabled = true;

std::uint32_t sDiag0145LogCount = 0;
constexpr std::uint32_t kDiag0145LogLimit = 50;

HOOK_DEFINE_TRAMPOLINE(Diag0145MarioDamage) {
    static void Callback(void* actor, std::uint16_t damageArg) {
        const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        if (!kDiag0145Enabled || !IsValidMarioActor(actor)) {
            Orig(actor, damageArg);
            return;
        }

        const std::uintptr_t actorAddr = reinterpret_cast<std::uintptr_t>(actor);
        void* p1 = GetFirstPlayer();
        void* p2 = GetSecondPlayer();
        const char* playerTag = (actor == p1) ? "P1(Mario)" : ((actor == p2) ? "P2(Luigi)" : "Unknown");

        const int oldHp = *reinterpret_cast<const int*>(actorAddr + 0x528);
        const float* actorVel = reinterpret_cast<const float*>(actorAddr + 0x38);

        const std::uintptr_t inner = *reinterpret_cast<const std::uintptr_t*>(actorAddr + 0x3C0);
        const float* coreVel = IsGuestPointer(inner) ? reinterpret_cast<const float*>(inner + 0x14C) : nullptr;
        const std::uint8_t invulnBefore = IsGuestPointer(inner) ? *reinterpret_cast<const std::uint8_t*>(inner + 0x8F0) : 0;

        const std::uintptr_t spine = *reinterpret_cast<const std::uintptr_t*>(actorAddr + 0x60);
        const void* nerve = IsGuestPointer(spine) ? *reinterpret_cast<const void**>(spine + 0x8) : nullptr;

        Orig(actor, damageArg);

        const int newHp = *reinterpret_cast<const int*>(actorAddr + 0x528);
        const std::uint32_t pending = *reinterpret_cast<const std::uint32_t*>(actorAddr + 0x530);
        const std::uint16_t dmgState = *reinterpret_cast<const std::uint16_t*>(actorAddr + 0x588);
        const std::uint8_t invulnAfter = IsGuestPointer(inner) ? *reinterpret_cast<const std::uint8_t*>(inner + 0x8F0) : 0;

        if (sDiag0145LogCount < kDiag0145LogLimit) {
            ++sDiag0145LogCount;
            const std::uintptr_t base = exl::util::modules::GetTargetStart();
            const bool inMain = caller >= base && caller < base + 0x1000000;
            Logging.Log("[GalaxyCoop:smg1] DIAG-0181 [%s] caller=%s0x%lx arg=%u hp=%d->%d vel=(%.1f,%.1f,%.1f) coreVel=(%.1f,%.1f,%.1f) pending=%u state=%u inv=%u->%u nrv=%p sample=%u/%u",
                        playerTag,
                        inMain ? "main+" : "mod:",
                        static_cast<unsigned long>(inMain ? caller - base : caller),
                        damageArg,
                        oldHp, newHp,
                        actorVel ? static_cast<double>(actorVel[0]) : 0.0,
                        actorVel ? static_cast<double>(actorVel[1]) : 0.0,
                        actorVel ? static_cast<double>(actorVel[2]) : 0.0,
                        coreVel ? static_cast<double>(coreVel[0]) : 0.0,
                        coreVel ? static_cast<double>(coreVel[1]) : 0.0,
                        coreVel ? static_cast<double>(coreVel[2]) : 0.0,
                        pending,
                        dmgState,
                        invulnBefore, invulnAfter,
                        nerve,
                        static_cast<unsigned>(sDiag0145LogCount),
                        static_cast<unsigned>(kDiag0145LogLimit));

            LogDiag0144Flags("damage-hit");

        }
    }
};

// DIAG-0182 (EVD-0237, TASK-0024): enemies (Karikari, Kuribo, Kabokuri) leave
// MarioActor+0xF20 set, pausing co-op camera and bubbles. Question: when
// +0xF20 stays set, is the rush-active byte +0xF38 (set 1 by 0x11C0B0) or the
// release byte +0xF39 (0x1901C4 writes halfword 0x100) telling it apart, and
// which caller of the producer 0x11BEB4 bound it? Read-only.
constexpr bool kDiag0182BindingProbeEnabled = false; // fulfilled: EVD-0238/EVD-0239
constexpr std::uintptr_t kDiag0182BindProducer = 0x11BEB4; // opening stp/stp/mov/ldr, EVD-0198
constexpr std::uint32_t kDiag0182ProducerLimit = 32;
constexpr std::uint32_t kDiag0182ChangeLimit = 96;
std::uint32_t sDiag0182ProducerLogs = 0, sDiag0182ChangeLogs = 0;

struct BindingSnapshot {
    std::uintptr_t sensor = 0;
    std::uint8_t active = 0, release = 0;
    std::uint32_t heldTicks = 0;
};
BindingSnapshot sDiag0182Last[2];

const char* Diag0182Tag(void* actor) {
    if (actor == GetFirstPlayer()) return "p1";
    if (actor == GetSecondPlayer()) return "p2";
    return "p?";
}
// Sensor +0 is the sensor type the producer switches on; +0x30 is the host.
void Diag0182Sensor(std::uintptr_t sensor, std::uintptr_t base, int* type, std::uintptr_t* hostVt) {
    *type = -1; *hostVt = 0;
    if (!IsGuestPointer(sensor)) return;
    *type = *reinterpret_cast<const int*>(sensor);
    const auto host = *reinterpret_cast<const std::uintptr_t*>(sensor + 0x30);
    if (!IsGuestPointer(host)) return;
    const auto vptr = *reinterpret_cast<const std::uintptr_t*>(host);
    *hostVt = vptr >= base ? vptr - base : 0;
}
std::uintptr_t Diag0182Nerve(std::uintptr_t actor, std::uintptr_t base) {
    const auto spine = *reinterpret_cast<const std::uintptr_t*>(actor + 0x60);
    if (!IsGuestPointer(spine)) return 0;
    const auto nerve = *reinterpret_cast<const std::uintptr_t*>(spine + 0x8);
    return nerve >= base ? nerve - base : 0;
}

HOOK_DEFINE_TRAMPOLINE(Diag0182BindProducer) {
    static void Callback(void* actor) {
        const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        Orig(actor);
        if (!kDiag0182BindingProbeEnabled || !IsValidMarioActor(actor) || sDiag0182ProducerLogs >= kDiag0182ProducerLimit) return;
        ++sDiag0182ProducerLogs;
        const auto a = reinterpret_cast<std::uintptr_t>(actor);
        const auto base = exl::util::modules::GetTargetStart();
        const auto sensor = *reinterpret_cast<const std::uintptr_t*>(a + 0xF20);
        int type; std::uintptr_t hostVt;
        Diag0182Sensor(sensor, base, &type, &hostVt);
        Logging.Log("[GalaxyCoop:smg1] DIAG-0182 bind %s caller=main+0x%lx sensor=%p type=0x%x host_vt=0x%lx f38=%u f39=%u nrv=main+0x%lx sample=%u/%u kill=1",
            Diag0182Tag(actor), static_cast<unsigned long>(caller - base), reinterpret_cast<void*>(sensor), type,
            static_cast<unsigned long>(hostVt), *reinterpret_cast<const std::uint8_t*>(a + 0xF38),
            *reinterpret_cast<const std::uint8_t*>(a + 0xF39), static_cast<unsigned long>(Diag0182Nerve(a, base)),
            sDiag0182ProducerLogs, kDiag0182ProducerLimit);
    }
};

// DIAG-0183 (TASK-0019 warp-to-power-up): the native stage restart 0x2506E0
// fills disc 5 with extra = 8 bytes read by getter 0x250770 from
// **(*(main+0xAA456178)+0x18); fill 0x21D33C copies extra to request +0x18,
// default global 0xAA4569C0. Question: are those 8 bytes two s32 (start
// MarioNo, zone id), and which values mark the PhantomGalaxy TeresaHouseZone
// start 141? Read-only, logs on change with P1 position.
constexpr bool kDiag0183RestartIdProbeEnabled = true;
constexpr std::uintptr_t kDiag0183SceneRoot = 0xAA456178; // same root as patch_camera
constexpr std::uintptr_t kDiag0183DefaultId = 0xAA4569C0;
constexpr std::uint32_t kDiag0183Limit = 48;
std::uint32_t sDiag0183Logs = 0;
bool sDiag0183Have = false, sDiag0183DefaultLogged = false;
std::uint64_t sDiag0183Last = 0;

bool Diag0183ReadRestartId(std::uintptr_t base, std::uint64_t* out) {
    const auto root = *reinterpret_cast<const std::uintptr_t*>(base + kDiag0183SceneRoot);
    if (!IsGuestPointer(root)) return false;
    const auto holder = *reinterpret_cast<const std::uintptr_t*>(root + 0x18);
    if (!IsGuestPointer(holder)) return false;
    const auto info = *reinterpret_cast<const std::uintptr_t*>(holder);
    if (!IsGuestPointer(info)) return false;
    // 0x250770 returns *info as a pointer; 0x2506E0 copies the id from it.
    const auto idInfo = *reinterpret_cast<const std::uintptr_t*>(info);
    if (!IsGuestPointer(idInfo)) return false;
    *out = *reinterpret_cast<const std::uint64_t*>(idInfo);
    return true;
}

}  // namespace

void TickDiag0183() {
    if (!kDiag0183RestartIdProbeEnabled || sDiag0183Logs >= kDiag0183Limit) return;
    const auto base = exl::util::modules::GetTargetStart();
    if (!sDiag0183DefaultLogged) {
        sDiag0183DefaultLogged = true;
        const std::uint64_t def = *reinterpret_cast<const std::uint64_t*>(base + kDiag0183DefaultId);
        Logging.Log("[GalaxyCoop:smg1] DIAG-0183 default restart id lo=%d hi=%d kill=1",
                    static_cast<int>(static_cast<std::uint32_t>(def)), static_cast<int>(def >> 32));
    }
    std::uint64_t id;
    if (!Diag0183ReadRestartId(base, &id)) { sDiag0183Have = false; return; }
    if (sDiag0183Have && id == sDiag0183Last) return;
    const std::uint64_t previous = sDiag0183Last;
    const bool hadPrevious = sDiag0183Have;
    sDiag0183Have = true;
    sDiag0183Last = id;
    ++sDiag0183Logs;
    float pos[3] = {};
    void* p1 = GetFirstPlayer();
    if (IsValidMarioActor(p1)) {
        for (int i = 0; i < 3; ++i) pos[i] = *reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(p1) + 0x14 + i * 4);
    }
    Logging.Log("[GalaxyCoop:smg1] DIAG-0183 restart id lo=%d hi=%d prev=(%d,%d) had_prev=%d p1=(%.0f,%.0f,%.0f) sample=%u/%u kill=1",
                static_cast<int>(static_cast<std::uint32_t>(id)), static_cast<int>(id >> 32),
                static_cast<int>(static_cast<std::uint32_t>(previous)), static_cast<int>(previous >> 32),
                hadPrevious ? 1 : 0, static_cast<double>(pos[0]), static_cast<double>(pos[1]),
                static_cast<double>(pos[2]), sDiag0183Logs, kDiag0183Limit);
}

void TickDiag0182() {
    if (!kDiag0182BindingProbeEnabled) return;
    const auto base = exl::util::modules::GetTargetStart();
    void* players[2] = {GetFirstPlayer(), GetSecondPlayer()};
    for (int i = 0; i < 2; ++i) {
        auto& last = sDiag0182Last[i];
        if (!IsValidMarioActor(players[i])) { last = {}; continue; }
        const auto a = reinterpret_cast<std::uintptr_t>(players[i]);
        BindingSnapshot now;
        now.sensor = *reinterpret_cast<const std::uintptr_t*>(a + 0xF20);
        now.active = *reinterpret_cast<const std::uint8_t*>(a + 0xF38);
        now.release = *reinterpret_cast<const std::uint8_t*>(a + 0xF39);
        now.heldTicks = now.sensor && now.sensor == last.sensor ? last.heldTicks + 1 : 0;
        const bool changed = now.sensor != last.sensor || now.active != last.active || now.release != last.release;
        // Also report a long-held binding once at 5 s so stale ones are named.
        if ((changed || now.heldTicks == 300) && sDiag0182ChangeLogs < kDiag0182ChangeLimit) {
            ++sDiag0182ChangeLogs;
            int type; std::uintptr_t hostVt;
            Diag0182Sensor(now.sensor ? now.sensor : last.sensor, base, &type, &hostVt);
            Logging.Log("[GalaxyCoop:smg1] DIAG-0182 p%d f20=%p->%p type=0x%x host_vt=0x%lx f38=%u->%u f39=%u->%u held=%u prev_held=%u nrv=main+0x%lx sample=%u/%u kill=1",
                i + 1, reinterpret_cast<void*>(last.sensor), reinterpret_cast<void*>(now.sensor), type,
                static_cast<unsigned long>(hostVt), last.active, now.active, last.release, now.release,
                now.heldTicks, last.heldTicks, static_cast<unsigned long>(Diag0182Nerve(a, base)),
                sDiag0182ChangeLogs, kDiag0182ChangeLimit);
        }
        last = now;
    }
}

void LogDiag0144Flags(const char* context) {
    if (!kDiag0144Enabled) {
        return;
    }
    const int p1Flag = ReadInnerEnableFlag(GetFirstPlayer());
    const int p2Flag = ReadInnerEnableFlag(GetSecondPlayer());

    static bool sHaveLast = false;
    static int sLastP1 = -2;
    static int sLastP2 = -2;
    if (!sHaveLast || p1Flag != sLastP1 || p2Flag != sLastP2) {
        sHaveLast = true;
        sLastP1 = p1Flag;
        sLastP2 = p2Flag;
        Logging.Log("[GalaxyCoop:smg1] DIAG-0144 [%s] p1.8f1=%d p2.8f1=%d",
                    context, p1Flag, p2Flag);
    }
}

void InstallDiagnostics(const galaxy::target::Identity& identity) {
    if (identity.game == galaxy::target::Game::Smg1) {
        if (kDiag0144Enabled) {
            Logging.Log("[GalaxyCoop:%s] DIAG-0144 flag probe armed (hook-free, reports from owned functions)",
                        galaxy::target::GameName(identity.game));
        }
        if (kDiag0145Enabled) {
            Diag0145MarioDamage::InstallAtOffset(kDiag0145MarioDamage);
            Logging.Log("[GalaxyCoop:%s] DIAG-0181 damage caller probe armed at 0x%lx",
                        galaxy::target::GameName(identity.game),
                        static_cast<unsigned long>(kDiag0145MarioDamage));
        }
        if (kDiag0182BindingProbeEnabled) {
            Diag0182BindProducer::InstallAtOffset(kDiag0182BindProducer);
            Logging.Log("[GalaxyCoop:%s] DIAG-0182 +0xF20 binding probe armed producer=0x%lx",
                        galaxy::target::GameName(identity.game),
                        static_cast<unsigned long>(kDiag0182BindProducer));
        }
        if (kDiag0183RestartIdProbeEnabled) {
            Logging.Log("[GalaxyCoop:%s] DIAG-0183 restart id probe armed (hook-free) kill=1",
                        galaxy::target::GameName(identity.game));
        }
    }
}

}  // namespace mod::patches
