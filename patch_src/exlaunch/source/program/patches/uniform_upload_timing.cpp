#include "uniform_upload_timing.hpp"
#if defined(GALAXY_UNIFORM_UPLOAD_TIMING_DIAG) && GALAXY_UNIFORM_UPLOAD_TIMING_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Uniform upload timing requires production owner
#endif
#if (defined(GALAXY_DESCRIPTOR_VALUE_EXPERIMENT) && GALAXY_DESCRIPTOR_VALUE_EXPERIMENT) || (defined(GALAXY_DRAW_ASSOCIATION_DIAG) && GALAXY_DRAW_ASSOCIATION_DIAG) || (defined(GALAXY_DYNAMIC_UNIFORM_EXPERIMENT) && GALAXY_DYNAMIC_UNIFORM_EXPERIMENT)
#error Fulfilled selector/association/uniform adapter must be omitted
#endif
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
#include <atomic>
namespace mod::patches {
extern "C" { UniformUploadBlock gGalaxyUniformUpload{}; }
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0343/0450: true10420 entry, 8register+4stack opaque arguments, u32 return;
// all3 direct return keys8504/D79C/103A0;26260->static global tableB9FA1710.
// No input/output, allocator, wait callback or uploaded storage dereference.
UniformUploadRow sRow{};bool sActive=false;unsigned sDepth=0;
std::atomic<std::uint64_t> sForeign{0};std::uint64_t sFirstForeign=0;
std::atomic<std::uintptr_t> sProducer{0}; // Opaque identity, never dereferenced.
std::uintptr_t Main(){return exl::util::modules::GetTargetStart();}
std::uintptr_t Thread(){std::uintptr_t v;asm volatile("mrs %0,tpidrro_el0":"=r"(v));return v;}
std::uint64_t Clock(){std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
bool Owned(){const auto owner=sProducer.load(std::memory_order_acquire);return owner&&Thread()==owner;}
template<class T>T Read(std::uintptr_t p,std::uintptr_t off){return *reinterpret_cast<const T*>(p+off);}
HOOK_DEFINE_TRAMPOLINE(UniformUploadCall){
    static std::uint32_t Callback(std::uintptr_t a0,std::uintptr_t a1,std::uintptr_t a2,std::uintptr_t a3,
        std::uintptr_t a4,std::uintptr_t a5,std::uintptr_t a6,std::uintptr_t a7,
        std::uintptr_t a8,std::uintptr_t a9,std::uintptr_t a10,std::uintptr_t a11){
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const auto forward=[&](){return Orig(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11);};
        if(!Owned()){sForeign.fetch_add(1,std::memory_order_relaxed);return forward();}
        if(!sActive)return forward();
        const auto base=Main();unsigned index=3;
        if(caller==base+0x8504)index=0;else if(caller==base+0xD79C)index=1;else if(caller==base+0x103A0)index=2;
        if(index==3||a1!=base+0xB9FA1710||sDepth){++sRow.invalidCalls;return forward();}
        ++sDepth;const bool timed=gGalaxyUniformUpload.mode==2;const auto start=timed?Clock():0;
        const auto result=forward();const auto elapsed=timed?Clock()-start:0;--sDepth;
        if(!sActive)return result; // A native wait may cross a frame boundary; that row refuses.
        ++sRow.calls;++sRow.callerCalls[index];sRow.returnMaskOr|=result;
        sRow.ticks+=elapsed;sRow.maximum=std::max(sRow.maximum,elapsed);return result;
    }
};
}
void UniformUploadSceneFrame(){
    // EVD0452:10420 is outside GPU owner. Existing2016C scene-wrapper entry
    // publishes its opaque producer token; no TLS object/field guessing.
    // One ledger writer and reader, both on this scene thread. Native GPU
    // consumer runs independently; these durations must not be added to it.
    const auto backend=gGalaxyPushFilter.backend,thread=Thread();
    if(!thread||!IsGuestPointer(backend)||Read<std::uintptr_t>(backend,0)!=Main()+0x889C68)return;
    auto& b=gGalaxyUniformUpload;
    std::uintptr_t empty=0;sProducer.compare_exchange_strong(empty,thread,std::memory_order_release);
    if(sProducer.load(std::memory_order_acquire)!=thread){sForeign.fetch_add(1,std::memory_order_relaxed);return;}
    b.thread=thread;++b.sequence;
    if(!sActive&&b.state!=1&&!b.request)return;
    const auto now=Clock(),frame=b.sequence;
    if(sActive){sActive=false;sRow.lastFrame=frame;sRow.end=now;
        sRow.invalidCalls+=sForeign.load(std::memory_order_relaxed)-sFirstForeign;
        bool valid=b.backend==backend&&b.thread==Thread()&&!sDepth&&frame==sRow.firstFrame+1&&now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&sRow.calls&&!sRow.invalidCalls;
        if(valid){sRow.completedSceneIntervals=1;valid=sRow.callerCalls[0]+sRow.callerCalls[1]+sRow.callerCalls[2]==sRow.calls&&sRow.maximum<=sRow.ticks&&sRow.ticks<=now-sRow.start;}
        if(valid)b.rows[b.count++]=sRow;else{b.rows[b.count]=sRow;++b.rejected;} // Preserve refused row; never average it.
        if(b.count==240||b.rejected)b.state=2;
    }
    if(b.request==1||b.request==2){b.mode=b.request;b.request=0;b.state=1;b.count=b.rejected=0;
        asm volatile("mrs %0,cntfrq_el0":"=r"(b.frequency));
    }
    if(b.state!=1||b.count>=240)return;
    if((b.backend&&b.backend!=backend)||(b.thread&&b.thread!=Thread())){b.state=2;return;}
    b.backend=backend;b.thread=Thread();sRow={};sRow.firstFrame=frame;sRow.start=now;sRow.mode=b.mode;sFirstForeign=sForeign.load(std::memory_order_relaxed);sActive=true;
    if(b.mode==2){const auto before=Clock();sRow.clockTicks=Clock()-before;sRow.clockCalls=1;}
}
void InstallUniformUploadTiming(){
    UniformUploadCall::InstallAtOffset(0x10420);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0206 ATT-0184 uniform upload timing installed default-idle producer-scene forward12 bounded=240 kill=1");
}
}
#endif
