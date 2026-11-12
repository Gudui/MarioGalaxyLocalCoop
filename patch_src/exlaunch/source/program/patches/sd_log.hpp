#pragma once

// ATT-0209 / EVD-0512: retail Horizon has no debug-string channel, so on a real
// Switch every Logging line is lost. Hardware builds (GALAXY_SD_LOG=1) also
// append each line, with seconds since the first line, to sd:/GalaxyCoop/smg1_log.txt.
// File I/O waits kOpenDelaySeconds after the first line so the game's own fs is
// up; earlier lines are buffered. The core takes its I/O as a parameter so the
// offline test drives it with a fake.

#include <lib/log/ilogger.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace mod::sdlog {

// Io provides: std::uint64_t Tick(); std::uint64_t TickHz(); bool Open();
// bool Write(std::int64_t offset, const char* data, std::size_t size) (write and
// flush); void Yield().
template <class Io>
class Core {
public:
    static constexpr std::size_t kBufferSize = 64 * 1024;
    static constexpr std::size_t kFlushBytes = 8 * 1024;
    static constexpr std::uint64_t kOpenDelaySeconds = 5;
    // ATT-0219 (EVD-0527): 1 s, with Poll() flushing even when no line comes.
    static constexpr std::uint64_t kFlushIntervalMs = 1000;
    static constexpr std::int64_t kMaxFileSize = 4 << 20;
    enum class State { Waiting, Open, Failed, Capped };

    void Log(Io& io, std::string_view text) {
        while (mLock.test_and_set(std::memory_order_acquire)) io.Yield();
        const std::uint64_t now = io.Tick();
        if (!mStarted) {
            mStarted = true;
            mStart = mLastFlush = now;
        }
        if (mState == State::Waiting || mState == State::Open) Append(io, now, text);
        if (mState == State::Waiting && now - mStart >= kOpenDelaySeconds * io.TickHz())
            mState = io.Open() ? State::Open : State::Failed;
        if (mState == State::Open && mUsed != 0 &&
            (mUsed >= kFlushBytes || now - mLastFlush >= kFlushIntervalMs * io.TickHz() / 1000))
            Flush(io, now);
        if (mState == State::Failed || mState == State::Capped) mUsed = 0;
        mLock.clear(std::memory_order_release);
    }

    // ATT-0219: timed flush from a per-frame caller, so lines written before a
    // stall or quit reach the card within kFlushIntervalMs. Skipped while a
    // Log() holds the lock; that Log() flushes on the same rule.
    void Poll(Io& io) {
        if (mLock.test_and_set(std::memory_order_acquire)) return;
        const std::uint64_t now = io.Tick();
        if (mState == State::Open && mUsed != 0 &&
            now - mLastFlush >= kFlushIntervalMs * io.TickHz() / 1000)
            Flush(io, now);
        mLock.clear(std::memory_order_release);
    }

    State GetState() const { return mState; }
    std::uint32_t Dropped() const { return mDropped; }

private:
    // One line: "[seconds] text\n". A line that does not fit is counted; the
    // count is reported as its own line ahead of the next line that fits.
    void Append(Io& io, std::uint64_t now, std::string_view text) {
        const double seconds = static_cast<double>(now - mStart) / static_cast<double>(io.TickHz());
        char note[64];
        int n = 0;
        if (mDropped != 0)
            n = std::snprintf(note, sizeof(note), "[%10.3f] SD log dropped %u lines\n",
                              seconds, static_cast<unsigned>(mDropped));
        char prefix[24];
        const int p = std::snprintf(prefix, sizeof(prefix), "[%10.3f] ", seconds);
        if (n < 0 || p <= 0) { ++mDropped; return; }
        const std::size_t line = static_cast<std::size_t>(p) + text.size() + 1;
        if (static_cast<std::size_t>(n) + line > kBufferSize - mUsed) { ++mDropped; return; }
        std::memcpy(mBuffer + mUsed, note, static_cast<std::size_t>(n));
        mUsed += static_cast<std::size_t>(n);
        mDropped = 0;
        std::memcpy(mBuffer + mUsed, prefix, static_cast<std::size_t>(p));
        std::memcpy(mBuffer + mUsed + p, text.data(), text.size());
        mBuffer[mUsed + line - 1] = '\n';
        mUsed += line;
    }

    void Flush(Io& io, std::uint64_t now) {
        if (mOffset + static_cast<std::int64_t>(mUsed) > kMaxFileSize) {
            static constexpr char kCap[] = "SD log size cap reached\n";
            io.Write(mOffset, kCap, sizeof(kCap) - 1);
            mState = State::Capped;
            return;
        }
        if (!io.Write(mOffset, mBuffer, mUsed)) { mState = State::Failed; return; }
        mOffset += static_cast<std::int64_t>(mUsed);
        mUsed = 0;
        mLastFlush = now;
    }

    std::atomic_flag mLock = ATOMIC_FLAG_INIT;
    State mState = State::Waiting;
    bool mStarted = false;
    std::uint64_t mStart = 0, mLastFlush = 0;
    std::int64_t mOffset = 0;
    std::size_t mUsed = 0;
    std::uint32_t mDropped = 0;
    char mBuffer[kBufferSize]{};
};

// Logger for exl::log::LoggerMgr (program/loggers.hpp), hardware builds only.
struct SdLogger : public exl::log::ILogger {
    virtual void LogRaw(std::string_view string) final;
};

// ATT-0219: Core::Poll on the SD log; a no-op without GALAXY_SD_LOG.
void Poll();

}  // namespace mod::sdlog
