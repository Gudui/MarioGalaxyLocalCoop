#include "frame_overlap.hpp"
#if defined(GALAXY_FRAME_OVERLAP) && GALAXY_FRAME_OVERLAP
#include "patch_splitscreen.hpp"
#if defined(GALAXY_PRODUCER_STALL_DIAG) && GALAXY_PRODUCER_STALL_DIAG
#if !(defined(GALAXY_FRAME_TIMELINE_DIAG) && GALAXY_FRAME_TIMELINE_DIAG)
#error "DIAG-0211 needs the DIAG-0208 frame timeline (GALAXY_FRAME_TIMELINE_DIAG)"
#endif
#include "frame_timeline.hpp"
#endif
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <cstdint>
namespace mod::patches {
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// PATCH-0192 / ATT-0195 (EVD-0472/0478/0480): split frames end in JSystem's
// GXDrawDone 97FD0, called from the frame-end watchdog wrapper 97E10. It writes
// BP 0x45000002, flushes and sleeps until GraphicsEventHanlder runs 4BFB30 for
// the consumer's draw-done packet. The producer parsed that command ~2.2 ms
// earlier and idles meanwhile. In split, the frame-end call now waits only until
// the producer's read pointer in the FIFO segment holding the command passes it
// (5048 advances segment+0xA0 after 5690 has parsed the chunk), so the consumer
// tail overlaps the next frame. Later stages keep their own flow control: the
// 2 MiB packet queue (91FC) and the 9 MiB vertex ring (544B04) block on space.
// Only 97FD0 and 4BF900 emit draw-done (whole-text scan) and the consumer posts
// in order, so issue/post counts tell 4BFB30 which post belongs to an early
// frame: it still runs the game callback but leaves the flag that strict waits
// (99D68, 32A4B4, merged frames) use.
// Replace hooks: 97FD0 and 4BF900 open with ADRP, 4BFB30 has ADRP at +0xC and
// is reached only through its callback slot; nothing branches into bytes 4..15
// of any (ghidra_overlap_sites_20261013). 5048 opens with stack setup and has
// one caller, 4E0490 (ghidra_overlap_sites2_20261013).
constexpr bool kFrameOverlapEnabled=true;
constexpr std::uintptr_t kDrawDone=0x97FD0,kSetDrawDone=0x4BF900,kDoneCallback=0x4BFB30,kProducerStep=0x5048;
constexpr std::uintptr_t kFrameEndReturn=0x97E84;  // bl 97FD0 at 97E80 in 97E10
constexpr std::uintptr_t kFifoBegin=0x4C1C40,kFifoByte=0x4C1C70,kFifoWord=0x4C1CB0,kGxFlush=0x99580;
constexpr std::uintptr_t kInitLightEvent=0x5F8C90,kSignalLightEvent=0x5F8CA0,kWaitLightEvent=0x5F8CB0,
                         kClearLightEvent=0x5F8CC0,kLockMutex=0x5F8D20,kUnlockMutex=0x5F8D40;
constexpr std::uintptr_t kDoneFlag=0xAA480C10,kDoneEvent=0xAA480BF8,kUserCallback=0xAA480C20,kCallbackMutex=0x953070;
constexpr std::uintptr_t kFifoSegment=0xAA6816A8,kFifoCursor=0xAA681680,kSegmentRead=0xA0,kSegmentBase=0x88;
constexpr std::uint32_t kDrawDoneCommand=0x45000002;
constexpr unsigned kRing=64,kLogEvery=600,kMaxLogs=40;
std::uintptr_t Base(){return exl::util::modules::GetTargetStart();}
template<class F>F Fn(std::uintptr_t offset){return reinterpret_cast<F>(Base()+offset);}
void* At(std::uintptr_t offset){return reinterpret_cast<void*>(Base()+offset);}
std::uint8_t* Flag(){return reinterpret_cast<std::uint8_t*>(Base()+kDoneFlag);}
std::uint64_t Clock(){std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
void Signal(void* event){Fn<void(*)(void*)>(kSignalLightEvent)(event);}
void Wait(void* event){Fn<void(*)(void*)>(kWaitLightEvent)(event);}
void Clear(void* event){Fn<void(*)(void*)>(kClearLightEvent)(event);}
void WriteDrawDone(){
    Fn<void(*)()>(kFifoBegin)();Fn<void(*)(std::uint8_t)>(kFifoByte)(0x61);
    Fn<void(*)()>(kFifoBegin)();Fn<void(*)(std::uint32_t)>(kFifoWord)(kDrawDoneCommand);
}
// Draw-done accounting: issue order is post order.
std::atomic<std::uint32_t> sIssued{0},sPosted{0};
std::atomic<bool> sEarly[kRing]{};
std::atomic<std::uint64_t> sReleaseTick[kRing]{};
std::atomic<bool> sBroken{false};
// Split state of the last frame end, read by the GX FIFO sync thread.
std::atomic<bool> sSplitFrame{false};
// PATCH-0194 counters (sync thread decisions in split, enables, disables).
std::atomic<std::uint32_t> sSlackEnables{0},sSlackDisables{0},sSlackSplit{0};
// Early wait handshake with the producer; the event uses the SDK layout.
alignas(16) std::uint8_t sEvent[0x40]{};
bool sEventReady=false;
std::atomic<bool> sWaiting{false};
std::atomic<std::uintptr_t> sWaitSegment{0},sWaitEnd{0};
// Log-only counters: main thread, plus saved time from the relay thread.
std::uint64_t sFrequency=0,sWaitTicks=0;
std::uint32_t sFrames=0,sReleased=0,sFallback=0,sLogs=0;
std::atomic<std::uint64_t> sSavedTicks{0},sSavedMax{0};
std::atomic<std::uint32_t> sSavedCount{0};
bool Reached(std::uintptr_t segment,std::uintptr_t end){
    return segment&&__atomic_load_n(reinterpret_cast<const std::uintptr_t*>(segment+kSegmentRead),__ATOMIC_ACQUIRE)>=end;
}
double Ms(std::uint64_t ticks,std::uint32_t n){
    return n?static_cast<double>(ticks)*1000.0/static_cast<double>(sFrequency)/n:0.0;
}
void LogStats(){
    if(sLogs>=kMaxLogs)return;
    ++sLogs;
    const auto savedTicks=sSavedTicks.exchange(0),savedMax=sSavedMax.exchange(0);
    const auto savedCount=sSavedCount.exchange(0);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0192 frame overlap frames=%u released=%u fallback=%u wait=%.2fms "
        "saved=(%.2f,%.2f,%u) issued=%u posted=%u broken=%d sync=(%u,%u,%u) kill=1",
        sFrames,sReleased,sFallback,Ms(sWaitTicks,sFrames),Ms(savedTicks,savedCount),Ms(savedMax,1),savedCount,
        sIssued.load(),sPosted.load(),sBroken.load()?1:0,
        sSlackSplit.load(),sSlackEnables.load(),sSlackDisables.load());
    sFrames=sReleased=sFallback=0;sWaitTicks=0;
}
void EarlyWait(std::uint32_t seq,std::uintptr_t segment,std::uintptr_t end){
    if(!sEventReady){Fn<void(*)(void*,bool,int)>(kInitLightEvent)(sEvent,false,0);sEventReady=true;}
    const bool valid=segment&&end>=*reinterpret_cast<const std::uintptr_t*>(segment+kSegmentBase);
    const auto start=Clock();
    sWaitSegment.store(valid?segment:0,std::memory_order_relaxed);
    sWaitEnd.store(end,std::memory_order_relaxed);
    sWaiting.store(true,std::memory_order_seq_cst);
    Clear(sEvent);
    bool released=false;
    for(;;){
        if(valid&&Reached(segment,end)){released=true;break;}
        // Fallback: the consumer already posted it (or a later one).
        if(sPosted.load(std::memory_order_acquire)>seq)break;
        Wait(sEvent);Clear(sEvent);
    }
    sWaiting.store(false,std::memory_order_seq_cst);
    const auto now=Clock();
    sReleaseTick[seq%kRing].store(released?now:0,std::memory_order_release);
    ++sFrames;sWaitTicks+=now-start;
    if(released)++sReleased;else ++sFallback;
    if(sFrames>=kLogEvery)LogStats();
}
HOOK_DEFINE_REPLACE(OverlapDrawDone){
    static void Callback(){
        // A replace hook branches without link, so x30 is the caller's return.
        const auto ret=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const bool frameEnd=ret==Base()+kFrameEndReturn;
        if(frameEnd)sSplitFrame.store(IsCoopSplitActive(),std::memory_order_relaxed);
        const bool early=!sBroken.load(std::memory_order_relaxed)&&frameEnd&&sSplitFrame.load(std::memory_order_relaxed);
        __atomic_store_n(Flag(),0,__ATOMIC_RELAXED);
        const auto seq=sIssued.fetch_add(1,std::memory_order_acq_rel);
        sEarly[seq%kRing].store(early,std::memory_order_release);
        WriteDrawDone();
        // 4C1C40 left the open segment and cursor just past the command.
        const auto segment=*reinterpret_cast<const std::uintptr_t*>(Base()+kFifoSegment);
        const auto end=*reinterpret_cast<const std::uintptr_t*>(Base()+kFifoCursor);
        Fn<void(*)()>(kGxFlush)();
        if(early){EarlyWait(seq,segment,end);return;}
        auto* const event=At(kDoneEvent);
        Clear(event);
        while(!__atomic_load_n(Flag(),__ATOMIC_ACQUIRE)&&sPosted.load(std::memory_order_acquire)<=seq){Wait(event);Clear(event);}
    }
};
HOOK_DEFINE_REPLACE(OverlapSetDrawDone){
    static void Callback(){
        __atomic_store_n(Flag(),0,__ATOMIC_RELAXED);
        const auto seq=sIssued.fetch_add(1,std::memory_order_acq_rel);
        sEarly[seq%kRing].store(false,std::memory_order_release);
        WriteDrawDone();
        Fn<void(*)()>(kGxFlush)();
    }
};
// GraphicsEventHanlder thread, once per consumer draw-done post.
HOOK_DEFINE_REPLACE(OverlapDoneCallback){
    static void Callback(){
        const auto seq=sPosted.fetch_add(1,std::memory_order_acq_rel);
        bool early=false;
        if(seq<sIssued.load(std::memory_order_acquire))early=sEarly[seq%kRing].exchange(false,std::memory_order_acq_rel);
        else sBroken.store(true,std::memory_order_relaxed);
        if(early){
            if(const auto released=sReleaseTick[seq%kRing].exchange(0,std::memory_order_acq_rel)){
                const auto saved=Clock()-released;
                sSavedTicks.fetch_add(saved,std::memory_order_relaxed);
                sSavedCount.fetch_add(1,std::memory_order_relaxed);
                auto max=sSavedMax.load(std::memory_order_relaxed);
                while(saved>max&&!sSavedMax.compare_exchange_weak(max,saved,std::memory_order_relaxed)){}
            }
        }else __atomic_store_n(Flag(),1,__ATOMIC_RELEASE);
        Signal(At(kDoneEvent));
        if(sWaiting.load(std::memory_order_acquire))Signal(sEvent);
        auto* const mutex=At(kCallbackMutex);
        Fn<void(*)(void*)>(kLockMutex)(mutex);
        if(const auto callback=*reinterpret_cast<void(* const*)()>(Base()+kUserCallback))callback();
        Fn<void(*)(void*)>(kUnlockMutex)(mutex);
    }
};
#if defined(GALAXY_PRODUCER_STALL_DIAG) && GALAXY_PRODUCER_STALL_DIAG
// DIAG-0211 / ATT-0196: why the producer idles (5048 parsed nothing, then
// SleepThread(100)) while main waits for it in split (EVD-0481). Producer
// thread only; reads the FIFO reader (producer+0x58) racily before each step:
// +0x698 consumed, +0x6B8/+0x6B9 limit flags with +0x6A8/+0x658, +0x6C0
// writer and +0x6C8 reader segments (+0x40 write, +0xA0 read).
enum Stall:unsigned{SEmpty,SUnaligned,SLimit,SBreak,SSegment,SParse,StallCount};
constexpr const char* kStallName[StallCount]={"empty","unaligned","limit","breakpt","segment","parse"};
constexpr unsigned kStallFrames=300,kStallLogs=40;
struct StallAcc{
    std::uint64_t busyTicks,idleTicks,idleMax,ticks[StallCount],critTicks,critRemain;
    std::uint32_t busy,idle,n[StallCount],crit,frames;
    // Idle steps while main waits: class and command byte at the read pointer.
    std::uint32_t critKind[StallCount],opN[32];
    std::uint64_t opTicks[32];
    std::uint8_t opByte[32];
};
StallAcc sStall{};
std::uint64_t sBusyTotal=0;  // DIAG-0213: running producer busy ticks in split frames
std::uint32_t sStallFrame=~0u,sStallLogs=0;
template<class T>T Read(std::uintptr_t a){return *reinterpret_cast<const volatile T*>(a);}
double StallMs(std::uint64_t t){return static_cast<double>(t)*1000.0/static_cast<double>(sFrequency)/kStallFrames;}
void StallLog(){
    const auto& a=sStall;
    const double us=a.idle?static_cast<double>(a.idleTicks)*1e6/static_cast<double>(sFrequency)/a.idle:0.0;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0211 producer idle frames=%u busy=(%.2f,%u) idle=(%.2f,%u,avg_us=%.0f,max_us=%.0f) "
        "%s=(%.2f,%u) %s=(%.2f,%u) %s=(%.2f,%u) %s=(%.2f,%u) %s=(%.2f,%u) %s=(%.2f,%u) main_waiting=(%.2f,%u) kill=1",
        a.frames,StallMs(a.busyTicks),a.busy,StallMs(a.idleTicks),a.idle,us,
        static_cast<double>(a.idleMax)*1e6/static_cast<double>(sFrequency),
        kStallName[0],StallMs(a.ticks[0]),a.n[0],kStallName[1],StallMs(a.ticks[1]),a.n[1],
        kStallName[2],StallMs(a.ticks[2]),a.n[2],kStallName[3],StallMs(a.ticks[3]),a.n[3],
        kStallName[4],StallMs(a.ticks[4]),a.n[4],kStallName[5],StallMs(a.ticks[5]),a.n[5],StallMs(a.critTicks),a.crit);
    // Four command classes (byte>>3) costing the most idle while main waits.
    unsigned top[4]={32,32,32,32};
    for(unsigned k=0;k<4;++k)for(unsigned o=0;o<32;++o){
        bool used=false;for(unsigned j=0;j<k;++j)used|=top[j]==o;
        if(!used&&a.opN[o]&&(top[k]==32||a.opTicks[o]>a.opTicks[top[k]]))top[k]=o;
    }
    auto ms=[&](unsigned k){return top[k]<32?StallMs(a.opTicks[top[k]]):0.0;};
    auto n=[&](unsigned k){return top[k]<32?a.opN[top[k]]:0u;};
    auto b=[&](unsigned k){return top[k]<32?a.opByte[top[k]]:0u;};
    Logging.Log("[GalaxyCoop:smg1] DIAG-0211 main_waiting kinds=(%u,%u,%u,%u,%u,%u) remain_avg=%lu "
        "ops=(0x%02x:%.2f/%u,0x%02x:%.2f/%u,0x%02x:%.2f/%u,0x%02x:%.2f/%u) kill=1",
        a.critKind[0],a.critKind[1],a.critKind[2],a.critKind[3],a.critKind[4],a.critKind[5],
        a.crit?a.critRemain/a.crit:0ul,b(0),ms(0),n(0),b(1),ms(1),n(1),b(2),ms(2),n(2),b(3),ms(3),n(3));
}
std::uintptr_t StepDiag(std::uintptr_t producer,std::uintptr_t (*orig)(std::uintptr_t)){
    if(!FrameTimelineSplit())return orig(producer);
    const auto frame=FrameTimelineFrameId();
    if(frame!=sStallFrame){
        sStallFrame=frame;
        if(++sStall.frames>kStallFrames){
            --sStall.frames;
            if(sStallLogs<kStallLogs){++sStallLogs;StallLog();}
            sStall=StallAcc{};sStall.frames=1;
        }
    }
    const auto reader=Read<std::uintptr_t>(producer+0x58);
    const auto consumed=Read<std::uint64_t>(reader+0x698);
    const auto writer=Read<std::uintptr_t>(reader+0x6C0),segment=Read<std::uintptr_t>(reader+0x6C8);
    const auto remain=Read<std::uintptr_t>(segment+0x40)-Read<std::uintptr_t>(segment+0xA0);
    const bool limited=Read<std::uint8_t>(reader+0x6B9)&&Read<std::uint64_t>(reader+0x658)<=consumed;
    const bool breakpt=Read<std::uint8_t>(reader+0x6B8)&&Read<std::uint64_t>(reader+0x6A8)<=consumed;
    const auto op=remain?Read<std::uint8_t>(Read<std::uintptr_t>(segment+0xA0)):0;
    const bool waiting=sWaiting.load(std::memory_order_relaxed);
    const auto start=Clock();
    const auto result=orig(producer);
    const auto ticks=Clock()-start;
    auto& a=sStall;
    if(Read<std::uint64_t>(reader+0x698)!=consumed){a.busyTicks+=ticks;++a.busy;sBusyTotal+=ticks;return result;}
    const unsigned kind=limited?SLimit:breakpt?SBreak:segment!=writer?SSegment:!remain?SEmpty:remain<32?SUnaligned:SParse;
    a.idleTicks+=ticks;++a.idle;if(ticks>a.idleMax)a.idleMax=ticks;
    a.ticks[kind]+=ticks;++a.n[kind];
    if(waiting){
        a.critTicks+=ticks;++a.crit;++a.critKind[kind];a.critRemain+=remain;
        a.opTicks[op>>3]+=ticks;++a.opN[op>>3];a.opByte[op>>3]=op;
    }
    return result;
}
#endif
// PATCH-0194 / ATT-0198 (EVD-0482/0483): the GX FIFO sync thread 41170
// (object AA456198, created by 1E02CC) receives the CPU FIFO pointer that 994F0
// posts before each GXSetDrawSync 99680 and the token value that the draw-sync
// callback 9BFFC posts when the consumer executes it. Outstanding pointers sit
// in a ring (+0x798: entries, cap +8, read +0xC, write +0x10). Natively, with
// two outstanding it sets GXEnableBreakPt 41390 at the newer one, so the
// producer (which honours the breakpoint through CP_CR -> D1700 -> D1744) stops
// one sync segment ahead of the consumer: ~1.5-1.9 ms of producer sleep per
// split frame while main waits (DIAG-0211). Removing the breakpoint entirely
// (PATCH-0193) let the producer run ~200 ms ahead and the consumer collapsed
// (12 fps, EVD-0483), so the limit stays; in split it allows one more segment.
// The replacement keeps the native message protocol, ring arithmetic and
// handshake (signal +0x7A8 after every pointer); 41170 is a thread entry whose
// first 16 bytes no branch targets (ghidra_fifo_sync_thread_20261013).
constexpr bool kSplitSyncSlackEnabled=true;
constexpr unsigned kSplitSyncSlack=2;
constexpr std::uintptr_t kSyncThread=0x41170,kReceiveMessage=0x412D0,kEnableBreakPt=0x41390,kDisableBreakPt=0x41330;
constexpr std::uint64_t kSyncExit=0x10000;
struct SyncRing{std::uint64_t* entries;std::int32_t cap;std::uint32_t read,write;};
std::uint32_t RingCount(const SyncRing& r){
    const std::uint32_t size=static_cast<std::uint32_t>(r.cap)+1;
    return r.write>=r.read?r.write-r.read:r.write+size-r.read;
}
std::uint32_t RingNext(const SyncRing& r,std::uint32_t i){return i+1>=static_cast<std::uint32_t>(r.cap)+1?0:i+1;}
HOOK_DEFINE_REPLACE(SplitSyncThread){
    static std::uintptr_t Callback(std::uintptr_t sync){
        auto receive=Fn<std::uint32_t(*)(std::uintptr_t,std::uint64_t*,int)>(kReceiveMessage);
        auto enable=Fn<void(*)(std::uint64_t)>(kEnableBreakPt);
        auto disable=Fn<void(*)()>(kDisableBreakPt);
        for(;;){
            std::uint64_t message=0;
            receive(sync+0x688,&message,1);
            auto& ring=**reinterpret_cast<SyncRing**>(sync+0x798);
            const bool split=sSplitFrame.load(std::memory_order_relaxed);
            const unsigned slack=split?kSplitSyncSlack:1;
            if(split)sSlackSplit.fetch_add(1,std::memory_order_relaxed);
            if(message>kSyncExit){
                ring.entries[ring.write]=message;
                ring.write=RingNext(ring,ring.write);
                if(RingCount(ring)==slack+1){enable(message);sSlackEnables.fetch_add(1,std::memory_order_relaxed);}
                Signal(reinterpret_cast<void*>(sync+0x7A8));
                continue;
            }
            if(message==kSyncExit)return 0;
            ring.read=RingNext(ring,ring.read);
            const auto count=RingCount(ring);
            if(!count)continue;
            if(count<=slack){disable();sSlackDisables.fetch_add(1,std::memory_order_relaxed);}
            else{
                auto index=ring.read;
                for(unsigned k=0;k<slack;++k)index=RingNext(ring,index);
                enable(ring.entries[index]);sSlackEnables.fetch_add(1,std::memory_order_relaxed);
            }
        }
    }
};
// Producer thread: one 5048 call parses one FIFO chunk and then advances the
// segment read pointer.
HOOK_DEFINE_TRAMPOLINE(OverlapProducerStep){
    static std::uintptr_t Callback(std::uintptr_t producer){
#if defined(GALAXY_PRODUCER_STALL_DIAG) && GALAXY_PRODUCER_STALL_DIAG
        const auto result=StepDiag(producer,[](std::uintptr_t p){return Orig(p);});
#else
        const auto result=Orig(producer);
#endif
        if(sWaiting.load(std::memory_order_acquire)&&
           Reached(sWaitSegment.load(std::memory_order_relaxed),sWaitEnd.load(std::memory_order_relaxed))&&
           sWaiting.exchange(false,std::memory_order_acq_rel))
            Signal(sEvent);
        return result;
    }
};
}
#if defined(GALAXY_PRODUCER_STALL_DIAG) && GALAXY_PRODUCER_STALL_DIAG
std::uint64_t ProducerStallBusyTicks(){return sBusyTotal;}
#endif
void InstallFrameOverlap(){
    if(!kFrameOverlapEnabled)return;
    asm volatile("mrs %0,cntfrq_el0":"=r"(sFrequency));
    OverlapDrawDone::InstallAtOffset(kDrawDone);
    OverlapSetDrawDone::InstallAtOffset(kSetDrawDone);
    OverlapDoneCallback::InstallAtOffset(kDoneCallback);
    OverlapProducerStep::InstallAtOffset(kProducerStep);
    if(kSplitSyncSlackEnabled){
        SplitSyncThread::InstallAtOffset(kSyncThread);
        Logging.Log("[GalaxyCoop:smg1] PATCH-0194 split sync slack installed thread=0x41170 slack=%u native=1 kill=1",kSplitSyncSlack);
    }
    Logging.Log("[GalaxyCoop:smg1] PATCH-0192 frame overlap installed drawdone=0x97FD0 set=0x4BF900 callback=0x4BFB30 "
        "producer=0x5048 frame_end_ret=0x97E84 split_only=1 kill=1");
#if defined(GALAXY_PRODUCER_STALL_DIAG) && GALAXY_PRODUCER_STALL_DIAG
    Logging.Log("[GalaxyCoop:smg1] DIAG-0211 ATT-0196 producer idle classes installed step=0x5048 reader=+0x58 frames=%u kill=1",kStallFrames);
#endif
}
}
#endif
