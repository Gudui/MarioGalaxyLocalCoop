#include "draw_association.hpp"
#if defined(GALAXY_DRAW_ASSOCIATION_DIAG) && GALAXY_DRAW_ASSOCIATION_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Association capture requires production frame ownership
#endif
#if (defined(GALAXY_DYNAMIC_UNIFORM_EXPERIMENT) && GALAXY_DYNAMIC_UNIFORM_EXPERIMENT) || \
    (defined(GALAXY_VERTEX_BATCH_DIAG) && GALAXY_VERTEX_BATCH_DIAG) || \
    (defined(GALAXY_BINDING_TIMING_DIAG) && GALAXY_BINDING_TIMING_DIAG) || \
    (defined(GALAXY_DRAW_COMMAND_TIMING_DIAG) && GALAXY_DRAW_COMMAND_TIMING_DIAG)
#error Retired diagnostics must be omitted
#endif
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cstring>
#include <atomic>
namespace mod::patches {
extern "C" { DrawAssociationBlock gGalaxyDrawAssociation{}; }
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0343/345/0440 true entries, packet ABI; EVD0401/0402 aligned draw thunk.
// EVD0419 cached key/pipeline/IDs; EVD0422/0425 native selected buffers.
// subsdk4 SHA9d7b958f58dcfbe013b3dbd0f126096a6b85452249f09fb7841247e348c2f46d.
constexpr std::uintptr_t kIndexed=0x639C0,kOther=0x243A0,kDraw=0x5F9F10,kBackendVT=0x889C68;
bool sActive=false;
unsigned sDepth=0,sKind=0;
std::uintptr_t sBackend=0,sHeader=0,sAttributes=0; // active parent only, cleared on return
std::atomic<std::uint64_t> sForeign{0};
std::uint64_t sFirstForeign=0;
std::uintptr_t Main(){return exl::util::modules::GetTargetStart();}
std::uintptr_t Thread(){std::uintptr_t v;asm volatile("mrs %0,tpidrro_el0":"=r"(v));return v;}
template<class T> T Read(std::uintptr_t p,std::uintptr_t off=0){return *reinterpret_cast<const T*>(p+off);}
bool Owned(){return gGalaxyPushFilter.thread&&Thread()==gGalaxyPushFilter.thread;}
bool Backend(std::uintptr_t p){return IsGuestPointer(p)&&Read<std::uintptr_t>(p)==Main()+kBackendVT;}
bool Span(std::uintptr_t p,std::size_t bytes){return p>=kMinGuestPointer&&p<kMaxGuestPointer&&!(p&3)&&bytes<=kMaxGuestPointer-p;}
void Fail(std::uint64_t bit){gGalaxyDrawAssociation.flags|=bit;}
std::uint64_t Fingerprint(std::uintptr_t p,std::size_t size){
    std::uint64_t h=14695981039346656037ULL;
    const auto* bytes=reinterpret_cast<const std::uint8_t*>(p);
    for(std::size_t i=0;i<size;++i){h^=bytes[i];h*=1099511628211ULL;}
    return h; // Candidate prefilter only; collisions/equal outputs never inferred.
}
struct Parent {
    bool active;
    Parent(unsigned kind,std::uintptr_t backend,std::uintptr_t header,std::uintptr_t attrs):active(Owned()&&sActive){
        if(!active)return;
        ++gGalaxyDrawAssociation.parentCalls;
        if(sDepth||!Backend(backend)||backend!=gGalaxyDrawAssociation.backend||!Span(header,24)||
           (kind==0&&!Span(attrs,0x2A8))){Fail(1);active=false;return;}
        sDepth=1;sKind=kind;sBackend=backend;sHeader=header;sAttributes=attrs;
    }
    ~Parent(){if(active){sDepth=0;sBackend=sHeader=sAttributes=0;}}
};
bool Uniforms(DrawAssociationRow& row,std::uintptr_t backend){
    // Fresh native module anchor and classes every draw; no scene heap cache.
    const auto thunk=Read<std::uintptr_t>(Main()+0x919AE0);
    if(!IsGuestPointer(thunk)||thunk<0x1C92310)return false;
    const auto native=thunk-0x1C92310, root=backend+0x1886A0;
    const auto layoutHandle=Read<std::uintptr_t>(root,0x18);
    if(!IsGuestPointer(layoutHandle)||layoutHandle<0x50)return false;
    const auto layout=layoutHandle-0x50;
    if(Read<std::uintptr_t>(layout)!=native+0x262B888||Read<std::uint32_t>(layout,0x10C)!=4)return false;
    const auto bindings=Read<std::uintptr_t>(layout,0x118);
    if(!IsGuestPointer(bindings))return false;
    constexpr unsigned counts[4]={4,2,2,1};unsigned start=0;
    for(unsigned i=0;i<4;++i){
        const auto p=bindings+i*0x40;
        if(Read<std::uint32_t>(p)!=6||Read<std::uint32_t>(p,4)!=counts[i]||
           Read<std::uint32_t>(p,0xC)!=1||Read<std::uint32_t>(p,0x24)!=start)return false;
        start+=counts[i];
    }
    row.descriptorCursor=Read<std::uint32_t>(root,0x8108);
    if(row.descriptorCursor>=4096)return false;
    row.descriptor=Read<std::uintptr_t>(root,0x28+row.descriptorCursor*8);
    if(!IsGuestPointer(row.descriptor)||row.descriptor<0x50)return false;
    const auto descriptor=row.descriptor-0x50;
    if(Read<std::uintptr_t>(descriptor)!=native+0x262B868||Read<std::uintptr_t>(descriptor,0x108)!=layout)return false;
    const auto addresses=Read<std::uintptr_t>(descriptor,0x138), ranges=Read<std::uintptr_t>(descriptor,0x140);
    if(!IsGuestPointer(addresses)||!IsGuestPointer(ranges))return false;
    for(unsigned i=0;i<9;++i){
        const auto info=root+0x8030+i*24;
        const auto buffer=Read<std::uintptr_t>(info), offset=Read<std::uint64_t>(info,8),size=Read<std::uint64_t>(info,16);
        const auto expected=i<6?256ULL:i<8?512ULL:768ULL;
        const auto table=Main()+0xB9FA1710+0x1A8+i*0x80;
        if(!IsGuestPointer(buffer)||buffer<0x50||buffer!=Read<std::uintptr_t>(table)||
           Read<std::uintptr_t>(buffer-0x50)!=native+0x262B948)return false;
        const auto capacity=Read<std::uint64_t>(buffer,0xB0),gpu=Read<std::uint64_t>(buffer,0xA8),storage=Read<std::uintptr_t>(table,0x28);
        if(size!=expected||capacity!=expected*4096||offset%expected||offset>capacity-size||
           !gpu||gpu>UINT64_MAX-capacity||!Span(storage,capacity)||
           Read<std::uint64_t>(addresses,i*8)!=gpu+offset||Read<std::uint32_t>(ranges,i*4)!=size)return false;
        row.offsets[i]=offset;row.fingerprints[i]=Fingerprint(storage+offset,size);
    }
    return true;
}
void Record(std::uintptr_t command,std::uint32_t indices,std::uint32_t instances,std::uint32_t firstIndex,
            std::int32_t vertexOffset,std::uint32_t firstInstance){
    auto& b=gGalaxyDrawAssociation;
    if(!Owned()){sForeign.fetch_add(1,std::memory_order_relaxed);return;}
    if(!sActive)return;
    ++b.nativeCalls;
    if(sDepth!=1||!Backend(sBackend)||sBackend!=b.backend||b.count>=b.capacity){Fail(4);return;}
    const auto backend=sBackend;
    const auto bank=Read<std::uint32_t>(backend,0x1061F8);
    const auto cache=backend+0x1B4E30;
    const auto count=Read<std::uint32_t>(cache,0x9F0220),index=Read<std::uint32_t>(cache,0x9F0224);
    if(bank>=12||Read<std::uint32_t>(backend,0x128C750+bank*4)!=1||
       command!=Read<std::uintptr_t>(backend,0x106190+bank*8)||!count||count>8192||index>=count){Fail(8);return;}
    auto& row=b.rows[b.count];row={};
    row.kind=sKind;row.bank=bank;row.command=command;row.target=Read<std::uintptr_t>(backend,0x106210);
    row.pipeline=Read<std::uintptr_t>(backend,0x128C730);row.pipelineIndex=index;
    if(!row.pipeline||row.pipeline!=Read<std::uintptr_t>(cache,0x9E4220+index*8)){Fail(16);return;}
    const auto ids=cache+0x9CC200+index*16;
    row.vertexId=Read<std::uint32_t>(ids);row.fragmentId=Read<std::uint32_t>(ids,4);
    if(row.vertexId>=Read<std::uint32_t>(cache,0xC7C2D0)||row.fragmentId>=Read<std::uint32_t>(cache,0x10D7308)){Fail(32);return;}
    const auto key=cache+0x200+index*0x688;
    row.topology=Read<std::uint32_t>(key,0x684);row.keyFingerprint=Fingerprint(key,0x688);
    row.arguments[0]=indices;row.arguments[1]=instances;row.arguments[2]=firstIndex;
    row.arguments[3]=static_cast<std::uint32_t>(vertexOffset);row.arguments[4]=firstInstance;row.arguments[5]=Read<std::uint32_t>(backend,0x106124);
    std::memcpy(row.viewport,reinterpret_cast<const void*>(backend+0x6E0),24);
    row.scissor[0]=Read<std::uint32_t>(backend,0x868);row.scissor[1]=Read<std::uint32_t>(backend,0x86C);row.scissor[2]=Read<std::uint32_t>(backend,0x94C);
    if(sKind==0){
        std::memcpy(row.header,reinterpret_cast<const void*>(sHeader),24);
        std::memcpy(row.attributes,reinterpret_cast<const void*>(sAttributes),0x2A8);
        if(Read<std::uint32_t>(sHeader,8)!=indices){Fail(64);return;}
    }
    if(!Uniforms(row,backend)){Fail(128);return;}
    ++b.count;
}
HOOK_DEFINE_TRAMPOLINE(AssociationIndexed){
    static void Callback(std::uintptr_t backend,const void* header,const void* attributes){
        Parent scope(0,backend,reinterpret_cast<std::uintptr_t>(header),reinterpret_cast<std::uintptr_t>(attributes));
        Orig(backend,header,attributes);
    }
};
HOOK_DEFINE_TRAMPOLINE(AssociationOther){
    static void Callback(std::uintptr_t backend,const void* packet){
        Parent scope(1,backend,reinterpret_cast<std::uintptr_t>(packet),0);Orig(backend,packet);
    }
};
HOOK_DEFINE_TRAMPOLINE(AssociationDraw){
    static void Callback(std::uintptr_t command,std::uint32_t indices,std::uint32_t instances,std::uint32_t firstIndex,std::int32_t vertexOffset,std::uint32_t firstInstance){
        Record(command,indices,instances,firstIndex,vertexOffset,firstInstance);
        Orig(command,indices,instances,firstIndex,vertexOffset,firstInstance);
    }
};
}
void DrawAssociationBoundary(std::uintptr_t backend){
    if(!Owned()||!Backend(backend))return;
    auto& b=gGalaxyDrawAssociation;++b.sequence;
    if(sActive){
        sActive=false;b.lastFrame=Read<std::uint64_t>(backend,0x60C8);
        if(sForeign.load(std::memory_order_relaxed)!=sFirstForeign)Fail(2);
        const auto bank=Read<std::uint32_t>(backend,0x60C0);
        if(bank>1||backend!=b.backend||Thread()!=b.thread||sDepth||b.lastFrame!=b.firstFrame+1){Fail(256);}
        else b.completedDraws=Read<std::uint32_t>(backend,0x6020+(bank^1)*0x40+0x30);
        if(b.count!=b.nativeCalls||b.parentCalls!=b.nativeCalls||b.count!=b.completedDraws||!b.count){Fail(512);}
        b.state=2;
    }
    if(b.request!=1)return;
    b.request=0;b.state=1;b.count=b.flags=b.parentCalls=b.nativeCalls=b.completedDraws=0;
    b.backend=backend;b.thread=Thread();b.firstFrame=Read<std::uint64_t>(backend,0x60C8);b.lastFrame=0;
    sFirstForeign=sForeign.load(std::memory_order_relaxed);sActive=true;
}
void InstallDrawAssociation(){
    AssociationIndexed::InstallAtOffset(kIndexed);AssociationOther::InstallAtOffset(kOther);AssociationDraw::InstallAtOffset(kDraw);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0204 ATT-0184 draw association installed idle one-frame cap=2048 forward-all fingerprints-only kill=1");
}
}
#endif
