#include "decoder_unpack_timing.hpp"
#if defined(GALAXY_DECODER_UNPACK_TIMING_DIAG) && GALAXY_DECODER_UNPACK_TIMING_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Decoder timing requires qualified production GPU owner
#endif
#if (defined(GALAXY_UNIFORM_UPLOAD_TIMING_DIAG) && GALAXY_UNIFORM_UPLOAD_TIMING_DIAG) || \
    (defined(GALAXY_DRAW_ASSOCIATION_DIAG) && GALAXY_DRAW_ASSOCIATION_DIAG) || \
    (defined(GALAXY_DESCRIPTOR_VALUE_EXPERIMENT) && GALAXY_DESCRIPTOR_VALUE_EXPERIMENT) || \
    (defined(GALAXY_DYNAMIC_UNIFORM_EXPERIMENT) && GALAXY_DYNAMIC_UNIFORM_EXPERIMENT) || \
    (defined(GALAXY_BINDING_TIMING_DIAG) && GALAXY_BINDING_TIMING_DIAG) || \
    (defined(GALAXY_BACKEND_TIMING_DIAG) && GALAXY_BACKEND_TIMING_DIAG)
#error Fulfilled or overlapping draw diagnostics must be omitted
#endif
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
#include <atomic>
namespace mod::patches {
extern "C" { DecoderUnpackBlock gGalaxyDecoderUnpack{}; }
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0457/59/60: typed887C10 owner, plain1C740 context keys, byte queue.
// EVD0462: 17AD0 one opaque arg/u32return; Other243A0 two opaque args,
// Indexed639C0 three args. Relocatable first16B; return keys4694/18518/188C8.
// Time ONLY entry-to-draw-entry; never rewrite unpacking or skip calls/work.
constexpr std::uintptr_t kDecoder=0x17AD0,kOther=0x243A0,kIndexed=0x639C0;
constexpr std::uintptr_t kOwnerVT=0x887C10,kBackendVT=0x889C68;
DecoderUnpackRow sRow{};bool sActive=false;
unsigned sDepth=0,sKind=0,sChildren=0;
std::uint64_t sSelectedStart=0,sFirstForeign=0;
std::atomic<std::uint64_t> sForeign{0};
std::uintptr_t Main(){return exl::util::modules::GetTargetStart();}
std::uintptr_t Thread(){std::uintptr_t v;asm volatile("mrs %0,tpidrro_el0":"=r"(v));return v;}
std::uint64_t Clock(){std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
template<class T>T Read(std::uintptr_t p,std::uintptr_t off){return *reinterpret_cast<const T*>(p+off);}
bool Owned(){return gGalaxyPushFilter.thread&&Thread()==gGalaxyPushFilter.thread;}
bool Backend(std::uintptr_t p){return IsGuestPointer(p)&&Read<std::uintptr_t>(p,0)==Main()+kBackendVT;}
bool Context(std::uintptr_t p){
    if(!IsGuestPointer(p)||!IsGuestPointer(p+0x1C738))return false;
    const auto timer=Read<std::uintptr_t>(p,0x10);
    if(!IsGuestPointer(timer)||timer<0x4F2D58)return false;
    const auto owner=timer-0x4F2D58;
    if(!IsGuestPointer(owner)||Read<std::uintptr_t>(owner,0)!=Main()+kOwnerVT)return false;
    return Read<std::uintptr_t>(owner,0x3B0)==p&&
        Read<std::uintptr_t>(p,0)==owner+0x20&&Read<std::uintptr_t>(p,8)==owner+0x4F2CB8&&
        Read<std::uintptr_t>(p,0x18)==owner+0x4F2DF8&&
        Read<std::uintptr_t>(p,0x1C710)==gGalaxyPushFilter.backend&&Backend(gGalaxyPushFilter.backend);
}
void Foreign(){if(sActive&&!Owned())sForeign.fetch_add(1,std::memory_order_relaxed);}
void Child(std::uintptr_t backend,unsigned kind,std::uintptr_t caller){
    Foreign();if(!Owned()||!sActive)return;
    ++sRow.backendDraws;
    if(backend!=gGalaxyDecoderUnpack.backend||sDepth!=1||sKind!=kind||sChildren++||
       caller!=Main()+(kind==2?0x18518:0x188C8)){sRow.flags|=2;return;}
    if(gGalaxyDecoderUnpack.mode==2){
        const auto now=Clock();
        if(now<sSelectedStart){sRow.flags|=4;return;}
        const auto elapsed=now-sSelectedStart;
        sRow.preTicks+=elapsed;sRow.preMaximum=std::max(sRow.preMaximum,elapsed);
    }
}
HOOK_DEFINE_TRAMPOLINE(DecoderUnpackCall){
    static std::uint32_t Callback(std::uintptr_t context){
        Foreign();if(!Owned()||!sActive)return Orig(context);
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        if(caller!=Main()+0x4694||!Context(context)){sRow.flags|=1;return Orig(context);}
        const auto read=Read<std::uint32_t>(context,0x1B3C),write=Read<std::uint32_t>(context,0x1B40);
        if((read|write)&3||read>0x1FE000||write>0x1FE000){sRow.flags|=8;return Orig(context);}
        if(read==write)return Orig(context);
        const auto storage=Read<std::uintptr_t>(context,0x1B30);
        if(!IsGuestPointer(storage)||Read<std::uint8_t>(context,0x1B38)!=1){sRow.flags|=8;return Orig(context);}
        const auto opcode=Read<std::uint32_t>(storage,read);
        if(opcode!=2&&opcode!=3)return Orig(context);
        if(sDepth){sRow.flags|=16;return Orig(context);}
        ++sDepth;sKind=opcode;sChildren=0;
        ++sRow.decoderDraws;if(opcode==2)++sRow.opcode2;else ++sRow.opcode3;
        sSelectedStart=gGalaxyDecoderUnpack.mode==2?Clock():0;
        const auto result=Orig(context);
        if(sActive&&sChildren!=1)sRow.flags|=32;
        --sDepth;sKind=0;sChildren=0;
        return result;
    }
};
HOOK_DEFINE_TRAMPOLINE(DecoderUnpackOther){
    static void Callback(std::uintptr_t backend,std::uintptr_t key){
        Child(backend,2,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
        Orig(backend,key);
    }
};
HOOK_DEFINE_TRAMPOLINE(DecoderUnpackIndexed){
    static void Callback(std::uintptr_t backend,const void* packet,const void* attrs){
        Child(backend,3,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
        Orig(backend,packet,attrs);
    }
};
}
void DecoderUnpackBoundary(std::uintptr_t backend){
    if(!Owned()||!Backend(backend))return;
    auto& b=gGalaxyDecoderUnpack;++b.sequence;
    if(!sActive&&b.state!=1&&!b.request)return;
    const auto now=Clock(),frame=Read<std::uint64_t>(backend,0x60C8);
    const auto bank=Read<std::uint32_t>(backend,0x60C0);
    if(sActive){
        sActive=false;sRow.lastFrame=frame;sRow.end=now;
        if(sForeign.load(std::memory_order_relaxed)!=sFirstForeign)sRow.flags|=64;
        bool valid=b.backend==backend&&b.thread==Thread()&&bank<2&&!sDepth&&
            frame==sRow.firstFrame+1&&now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&!sRow.flags;
        if(valid){
            sRow.draws=Read<std::uint32_t>(backend,0x6020+(bank^1)*0x40+0x30);
            valid=sRow.draws&&sRow.draws==sRow.decoderDraws&&sRow.draws==sRow.backendDraws&&
                sRow.opcode2+sRow.opcode3==sRow.draws&&sRow.preMaximum<=sRow.preTicks&&
                sRow.preTicks<=now-sRow.start&&sRow.mode==b.mode;
        }
        if(valid)b.rows[b.count++]=sRow;else{b.rows[b.count]=sRow;++b.rejected;}
        if(b.count==240||b.rejected)b.state=2;
    }
    if(b.request==1||b.request==2){
        b.mode=b.request;b.request=0;b.state=1;b.count=b.rejected=0;
        asm volatile("mrs %0,cntfrq_el0":"=r"(b.frequency));
    }
    if(b.state!=1||bank>1||b.count>=240)return;
    b.backend=backend;b.thread=Thread();sRow={};sFirstForeign=sForeign.load(std::memory_order_relaxed);
    sRow.firstFrame=frame;sRow.start=now;sRow.mode=b.mode;sActive=true;
    if(b.mode==2){const auto before=Clock();sRow.clockTicks=Clock()-before;sRow.clockPairs=1;}
}
void InstallDecoderUnpackTiming(){
    DecoderUnpackCall::InstallAtOffset(kDecoder);DecoderUnpackOther::InstallAtOffset(kOther);DecoderUnpackIndexed::InstallAtOffset(kIndexed);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0207 ATT-0186 decoder pre-draw timing installed default-idle forward-all bounded=240 kill=1");
}
}
#endif
