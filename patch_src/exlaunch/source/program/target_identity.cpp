#include "program/target_identity.hpp"
#include "program/loggers.hpp"
#include <cstring>

namespace galaxy::target {

namespace {
// SMG1 v1.3.1 main (SHA-256 76f1b4d8...05938): NSO header module id, the GNU
// build id of the module. Every hook offset belongs to this build; on a console
// with another game version they would patch unrelated code.
constexpr unsigned char kSmg1BuildId[20] = {
    0x9d, 0x08, 0xca, 0xdc, 0xbc, 0xc1, 0xce, 0xa4, 0x28, 0x38,
    0x17, 0x45, 0x29, 0x1f, 0x8b, 0xc2, 0xb0, 0xb1, 0x3f, 0x4e};

// The build id note ("GNU\0", type 3) lives in main's rodata.
bool MainHasBuildId(const unsigned char* id, std::size_t size) {
    const auto& rodata = exl::util::GetMainModuleInfo().m_Rodata;
    const auto* data = reinterpret_cast<const unsigned char*>(rodata.m_Start);
    for (std::size_t i = 0; i + size <= rodata.m_Size; ++i)
        if (data[i] == id[0] && std::memcmp(data + i, id, size) == 0) return true;
    return false;
}
}  // namespace

Identity DetectAndValidate() {
    const auto& buildId = galaxycoop::config::GetBuildIdentity();
    Identity id{buildId.game, buildId.executableSha256, false};

    if (id.game == Game::Smg1) {
        id.valid = MainHasBuildId(kSmg1BuildId, sizeof(kSmg1BuildId));
        Logging.Log("[GalaxyCoop:smg1] main build id 9d08cadcbcc1cea428381745291f8bc2b0b13f4e %s",
            id.valid ? "found" : "NOT found; refusing all patches");
    } else if (id.game == Game::Smg2) {
        // SMG2 is not yet configured for Switch ExeFS hooks
        id.valid = false;
    } else {
        id.valid = false;
    }

    return id;
}

const char* GameName(Game game) {
    switch (game) {
        case Game::Smg1: return "smg1";
        case Game::Smg2: return "smg2";
        default: return "unconfigured";
    }
}

}  // namespace galaxy::target
