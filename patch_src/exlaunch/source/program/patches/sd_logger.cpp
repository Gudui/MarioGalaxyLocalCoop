#include "sd_log.hpp"

#if defined(GALAXY_SD_LOG) && GALAXY_SD_LOG

#include <lib.hpp>
#include <nn/fs.hpp>

// Exported by the game's sdk module (dynsym, defined); not in exlaunch's headers.
namespace nn::fs {
Result MountSdCard(char const* mount);
Result DeleteFile(char const* path);
Result RenameFile(char const* from, char const* to);
}  // namespace nn::fs

namespace mod::sdlog {
namespace {

constexpr char kMount[] = "sd";
constexpr char kDirectory[] = "sd:/GalaxyCoop";
constexpr char kPath[] = "sd:/GalaxyCoop/smg1_log.txt";
constexpr char kPreviousPath[] = "sd:/GalaxyCoop/smg1_log_prev.txt";

struct HorizonIo {
    nn::fs::FileHandle file{};
    std::uint64_t Tick() { return svcGetSystemTick(); }
    std::uint64_t TickHz() { return 19200000; }
    // A real sleep, not svcSleepThread(0): that only yields to equal priority,
    // so a higher-priority logger would starve a lower-priority writer on its core.
    void Yield() { svcSleepThread(50000); }
    // Mount, keep the previous session's log, start an empty one.
    bool Open() {
        if (nn::fs::MountSdCardForDebug(kMount) != 0 && nn::fs::MountSdCard(kMount) != 0) return false;
        nn::fs::CreateDirectory(kDirectory);
        nn::fs::DeleteFile(kPreviousPath);
        nn::fs::RenameFile(kPath, kPreviousPath);
        if (nn::fs::CreateFile(kPath, 0) != 0) return false;
        return nn::fs::OpenFile(&file, kPath, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append) == 0;
    }
    bool Write(std::int64_t offset, const char* data, std::size_t size) {
        return nn::fs::WriteFile(file, offset, data, size,
                                 nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush)) == 0;
    }
};

HorizonIo sIo;
Core<HorizonIo> sCore;

}  // namespace

void SdLogger::LogRaw(std::string_view string) { sCore.Log(sIo, string); }
void Poll() { sCore.Poll(sIo); }

}  // namespace mod::sdlog

#else

namespace mod::sdlog {
void Poll() {}
}  // namespace mod::sdlog

#endif
