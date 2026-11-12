#include "lib.hpp"
#include "program/galaxy_config.hpp"
#include "program/loggers.hpp"
#include "program/target_identity.hpp"
#include "program/patches/patches.hpp"

extern "C" void exl_main(void* env, void* dynamic_context) {
    exl::hook::Initialize();

    const auto identity = galaxy::target::DetectAndValidate();
    if (!identity.valid) {
        Logging.Log("[GalaxyCoop] Aborting: target executable failed identity validation.");
        return;
    }

    Logging.Log("[GalaxyCoop:%s] Initializing modular patches...", galaxy::target::GameName(identity.game));
    mod::patches::InstallAll(identity);
    Logging.Log("[GalaxyCoop:%s] All modular patches installed successfully.", galaxy::target::GameName(identity.game));
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
