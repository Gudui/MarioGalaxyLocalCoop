#include "dynamic_uniform.hpp"
#if defined(GALAXY_DYNAMIC_UNIFORM_EXPERIMENT) && GALAXY_DYNAMIC_UNIFORM_EXPERIMENT
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Dynamic uniforms require the production owner
#endif
#if (defined(GALAXY_VERTEX_BATCH_DIAG) && GALAXY_VERTEX_BATCH_DIAG) || (defined(GALAXY_BINDING_TIMING_DIAG) && GALAXY_BINDING_TIMING_DIAG)
#error Fulfilled diagnostics must be omitted
#endif
#include "dynamic_uniform_plan.hpp"
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
#include <atomic>

namespace mod::patches {
extern "C" { DynamicUniformBlock gGalaxyDynamicUniform{}; }
// EVD0365/425: replace entire16B ADRP/LDR/ADD/BR import thunks. GOT untouched;
// helpers preserve PLT0's x16, including before first lazy resolution.
extern "C" std::int32_t GalaxyUniformCreateImport(std::uintptr_t,const void*,const void*,void*,std::uintptr_t);
extern "C" void GalaxyUniformUpdateImport(std::uintptr_t,std::uint32_t,const void*,std::uint32_t,const void*,std::uintptr_t);
extern "C" void GalaxyUniformBindImport(std::uintptr_t,std::uint32_t,std::uintptr_t,std::uint32_t,std::uint32_t,const void*,std::uint32_t,const void*,std::uintptr_t);
asm(".pushsection .text\n.balign 4\n.global GalaxyUniformCreateImport\n"
    "GalaxyUniformCreateImport:\nmov x16,x4\nldr x17,[x16]\nbr x17\n"
    ".balign 4\n.global GalaxyUniformUpdateImport\n"
    "GalaxyUniformUpdateImport:\nmov x16,x5\nldr x17,[x16]\nbr x17\n"
    ".balign 4\n.global GalaxyUniformBindImport\n"
    "GalaxyUniformBindImport:\nldr x16,[sp]\nldr x17,[x16]\nbr x17\n.popsection\n");
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938;
// subsdk4 SHA9d7b958f58dcfbe013b3dbd0f126096a6b85452249f09fb7841247e348c2f46d.
// EVD0425/426: arrays4/2/2/1, nine selected ring infos, native type8 numeric
// address route/template indices and copied command payload. Preserve every
// upload10420, ring reservation/retirement7658C, nonvertex operation and draw.
constexpr std::uintptr_t kLayoutThunk=0x5F9E60,kLayoutGot=0x919A70,kLayoutReturn=0x228BB4;
constexpr std::uintptr_t kPoolThunk=0x5F9E10,kPoolGot=0x919A48,kPoolReturn=0x2288A4;
constexpr std::uintptr_t kUpdateThunk=0x5F9F30,kUpdateGot=0x919AD8,kBindThunk=0x5F9F40,kBindGot=0x919AE0;
constexpr std::uintptr_t kIndexed=0x639C0,kOther=0x243A0,kBackendVT=0x889C68;
struct LayoutCreate { std::uint32_t type,pad;const void* next;std::uint32_t flags,count;const UniformBinding* bindings; };
struct PoolSize { std::uint32_t type,count; };
struct PoolCreate { std::uint32_t type,pad;const void* next;std::uint32_t flags,maxSets,count,pad2;const PoolSize* sizes; };
static_assert(sizeof(LayoutCreate)==32&&offsetof(LayoutCreate,bindings)==24);
static_assert(sizeof(PoolCreate)==40&&offsetof(PoolCreate,sizes)==32);
std::uintptr_t sNative=0; // Module base only, never a heap owner.
DynamicUniformRow sRow{};
bool sActive=false;
unsigned sDepth=0;
std::uintptr_t sDrawBackend=0; // Current draw argument only; cleared on return.
UniformDrawCache* sDrawCache=nullptr; // Points only to the active Parent stack.
std::uintptr_t sDrawLayout=0;
bool sDrawLayoutChecked=false;
std::atomic<std::uint64_t> sForeign{0};std::uint64_t sFirstForeign=0;
std::uintptr_t Main() { return exl::util::modules::GetTargetStart(); }
std::uintptr_t Thread() { std::uintptr_t v;asm volatile("mrs %0,tpidrro_el0":"=r"(v));return v; }
std::uint64_t Clock() { std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v; }
template<class T> T Read(std::uintptr_t p,std::uintptr_t off=0) { return *reinterpret_cast<const T*>(p+off); }
bool Pointer(const void* p) { return IsGuestPointer(reinterpret_cast<std::uintptr_t>(p)); }
bool Owned() { return gGalaxyPushFilter.thread&&Thread()==gGalaxyPushFilter.thread; }
bool Class(std::uintptr_t base,std::uintptr_t vt) { return sNative&&IsGuestPointer(base)&&Read<std::uintptr_t>(base)==sNative+vt; }
bool Backend(std::uintptr_t p) { return IsGuestPointer(p)&&Read<std::uintptr_t>(p)==Main()+kBackendVT; }
void Fail() { if(sActive){++sRow.failures;sRow.flags|=0x400;}gGalaxyDynamicUniform.shadowPassed=0; }
bool Fast() { return Owned()&&sActive&&sDepth==1&&gGalaxyDynamicUniform.mode==3&&gGalaxyDynamicUniform.shadowPassed; }
struct Timing {
    unsigned kind;bool active,timed;std::uint64_t start;
    Timing(unsigned k):kind(k),active(Owned()&&sActive),timed(active&&gGalaxyDynamicUniform.mode!=2),start(timed?Clock():0){}
    ~Timing(){if(active){auto& m=sRow.metric[kind];const auto d=timed?Clock()-start:0;m.ticks+=d;m.maximum=std::max(m.maximum,d);++m.calls;}}
};
bool Device(std::uintptr_t device) {
    if(!IsGuestPointer(device))return false;
    const auto dispatch=Read<std::uintptr_t>(device);
    if(!IsGuestPointer(dispatch)||Read<std::uint64_t>(dispatch)!=0x10ADED040410ADEDULL)return false;
    const auto create=Read<std::uintptr_t>(dispatch,0x1C8);
    if(!IsGuestPointer(create)||create<0x1626030)return false;
    const auto native=create-0x1626030;
    if(sNative&&sNative!=native)return false;
    sNative=native;return true;
}
bool VertexLayout(std::uintptr_t layout) {
    if(!Class(layout,0x262B888)||Read<std::uint32_t>(layout,0x10C)!=4)return false;
    const auto records=Read<std::uintptr_t>(layout,0x118);
    if(!IsGuestPointer(records))return false;
    const unsigned counts[4]={4,2,2,1};unsigned start=0;
    for(unsigned i=0;i<4;++i) {
        const auto r=records+i*0x40;
        if(Read<std::uint32_t>(r)!=8||Read<std::uint32_t>(r,4)!=counts[i]||Read<std::uint32_t>(r,0xC)!=1||
           Read<std::uint32_t>(r,0x24)!=start||Read<std::uint32_t>(r,0x38)!=start)return false;
        start+=counts[i];
    }
    return true;
}
bool VertexSet(std::uintptr_t handle) {
    if(!handle||!Class(handle-0x50,0x262B868))return false;
    const auto layout=Read<std::uintptr_t>(handle-0x50,0x108);
    if(Owned()&&sDepth&&sDrawCache) {
        if(!sDrawLayoutChecked) {
            sDrawLayoutChecked=true;sDrawLayout=0;
            if(Backend(sDrawBackend)) {
                const auto answer=Read<std::uintptr_t>(sDrawBackend,0x1886A0+0x18);
                if(answer&&VertexLayout(answer-0x50))sDrawLayout=answer-0x50;
            }
        }
        return sDrawLayout&&layout==sDrawLayout;
    }
    return VertexLayout(layout);
}
bool Payload(std::uintptr_t set,const std::uint64_t*& addresses,const std::uint32_t*& ranges) {
    if(!VertexSet(set))return false;
    addresses=reinterpret_cast<const std::uint64_t*>(Read<std::uintptr_t>(set-0x50,0x138));
    ranges=reinterpret_cast<const std::uint32_t*>(Read<std::uintptr_t>(set-0x50,0x140));
    return Pointer(addresses)&&Pointer(ranges);
}
bool Plan(UniformPlan& plan) {
    if(!Owned())return false;
    const auto backend=sDrawBackend;
    if(!Backend(backend))return false;
    const auto root=backend+0x1886A0;
    const auto layout=Read<std::uintptr_t>(root,0x18);
    if(!layout||!sDrawLayoutChecked||layout-0x50!=sDrawLayout)return false;
    for(unsigned i=0;i<9;++i) {
        plan.selected[i]=Read<UniformBufferInfo>(root,0x8030+i*24);
        const auto handle=plan.selected[i].buffer;
        if(!Class(handle-0x50,0x262B948)||handle!=Read<std::uintptr_t>(Main()+0xB9FA1710,0x1A8+i*0x80))return false;
        plan.base[i]=Read<std::uint64_t>(handle,0xA8);plan.capacity[i]=Read<std::uint64_t>(handle,0xB0);
    }
    return plan.Normalize();
}
bool DrawPlan() { return sDrawCache&&sDrawCache->Get([](UniformPlan& plan){return Plan(plan);}); }
void Update(std::uintptr_t device,unsigned count,const UniformWrite* writes,unsigned copies,const UniformCopy* copy) {
    GalaxyUniformUpdateImport(device,count,writes,copies,copy,Main()+kUpdateGot);
}
HOOK_DEFINE_REPLACE(UniformPoolCreate) {
    static std::int32_t Callback(std::uintptr_t device,const PoolCreate* info,const void* allocator,std::uintptr_t* output) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        if(caller!=Main()+kPoolReturn||!Pointer(info)||info->type!=33||info->next||info->count!=2||!Pointer(info->sizes)||!Pointer(output)||!Device(device))
            return GalaxyUniformCreateImport(device,info,allocator,output,Main()+kPoolGot);
        // Preserve original capacities/maxSets/flags, add9*4096 type8 entries.
        PoolSize sizes[3]={info->sizes[0],info->sizes[1],{8,9*4096}};
        if(sizes[0].type!=6||sizes[1].type!=7)return GalaxyUniformCreateImport(device,info,allocator,output,Main()+kPoolGot);
        auto adapted=*info;adapted.count=3;adapted.sizes=sizes;
        const auto result=GalaxyUniformCreateImport(device,&adapted,allocator,output,Main()+kPoolGot);
        if(!result)++gGalaxyDynamicUniform.poolsExtended;else ++gGalaxyDynamicUniform.creationFailures;
        return result;
    }
};
HOOK_DEFINE_REPLACE(UniformLayoutCreate) {
    static std::int32_t Callback(std::uintptr_t device,const LayoutCreate* info,const void* allocator,std::uintptr_t* output) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        if(caller!=Main()+kLayoutReturn||!gGalaxyDynamicUniform.poolsExtended||!Pointer(info)||info->type!=32||info->next||info->flags||info->count!=4||!Pointer(info->bindings)||!Pointer(output)||!Device(device))
            return GalaxyUniformCreateImport(device,info,allocator,output,Main()+kLayoutGot);
        UniformBinding bindings[4];const unsigned counts[4]={4,2,2,1};
        for(unsigned i=0;i<4;++i) {
            bindings[i]=info->bindings[i];
            if(bindings[i].binding!=i||bindings[i].type!=6||bindings[i].count!=counts[i]||bindings[i].stages!=1||bindings[i].samplers) {
                ++gGalaxyDynamicUniform.layoutsRefused;return GalaxyUniformCreateImport(device,info,allocator,output,Main()+kLayoutGot);
            }
            bindings[i].type=8;
        }
        auto adapted=*info;adapted.bindings=bindings;const auto old=*output;
        const auto result=GalaxyUniformCreateImport(device,&adapted,allocator,output,Main()+kLayoutGot);
        if(!result){++gGalaxyDynamicUniform.layoutsConverted;return result;}
        ++gGalaxyDynamicUniform.creationFailures;*output=old;
        //167E140 releases failed layout allocation before returning an error.
        return GalaxyUniformCreateImport(device,info,allocator,output,Main()+kLayoutGot);
    }
};
HOOK_DEFINE_REPLACE(UniformUpdate) {
    static void Callback(std::uintptr_t device,unsigned count,const UniformWrite* writes,unsigned copies,const UniformCopy* copy) {
        Timing timing(2);
        if(!Owned()||!sDepth||count>16||copies>16||(count&&!Pointer(writes))||(copies&&!Pointer(copy)))return Update(device,count,writes,copies,copy);
        bool vertexWrites[16]{},vertexCopies[16]{};std::uintptr_t vertex=0;bool unusual=false;
        for(unsigned i=0;i<count;++i)if((vertexWrites[i]=VertexSet(writes[i].set))){if(vertex&&vertex!=writes[i].set)unusual=true;vertex=writes[i].set;}
        for(unsigned i=0;i<copies;++i)if((vertexCopies[i]=VertexSet(copy[i].destination))){if(vertex&&vertex!=copy[i].destination)unusual=true;vertex=copy[i].destination;}
        const std::uint64_t* addresses=nullptr;const std::uint32_t* ranges=nullptr;
        //27AD0 has finished publishing selected infos before this API call.
        // Re-read on every vertex update, then share only with its later bind in
        // this same native draw. Native controls use the identical reader.
        if(vertex&&sDrawCache)sDrawCache->Invalidate();
        const bool planned=vertex&&!unusual&&DrawPlan();
        const bool optimize=Fast()&&planned&&Payload(vertex,addresses,ranges);
        const auto* plan=planned?&sDrawCache->plan:nullptr;
        UniformUpdatePlan rewrite;
        if(!rewrite.Build(count,writes,vertexWrites,copies,copy,vertexCopies,vertex,optimize?plan:nullptr,optimize&&plan->Stable(addresses,ranges))) {
            Fail();return Update(device,count,writes,copies,copy);
        }
        if(vertex&&sActive){++sRow.vertexUpdates;sRow.writesRemoved+=rewrite.removedWrites;sRow.copiesRemoved+=rewrite.removedCopies;sRow.initializations+=rewrite.initialized;}
        if(rewrite.writeCount||rewrite.copyCount)Update(device,rewrite.writeCount,rewrite.writes,rewrite.copyCount,rewrite.copies);
    }
};
HOOK_DEFINE_REPLACE(UniformBind) {
    static void Callback(std::uintptr_t command,unsigned point,std::uintptr_t layout,unsigned first,unsigned count,const std::uintptr_t* sets,unsigned dynamics,const std::uint32_t* originalOffsets) {
        Timing timing(3);
        const auto forward=[&](unsigned n,const std::uint32_t* offsets){GalaxyUniformBindImport(command,point,layout,first,count,sets,n,offsets,Main()+kBindGot);};
        if(point||first>3||!count||count>4-first||!Pointer(sets)||!Class(layout-0x50,0x262B8A8))return forward(dynamics,originalOffsets);
        std::uint32_t offsets[9]{};unsigned total=0;std::uintptr_t vertex=0;bool compatible=!dynamics;
        for(unsigned i=0;i<count;++i) {
            const auto set=first+i;const auto d=Read<std::uint8_t>(layout-0x50,0x208+set*0x1A0+0x198);
            if(set==3&&VertexSet(sets[i])) {
                compatible=compatible&&d==9&&Read<std::uintptr_t>(layout-0x50,0x100+set*8)==Read<std::uintptr_t>(sets[i]-0x50,0x108);
                vertex=sets[i];total+=d;
            }else if(d)compatible=false;
        }
        if(!vertex)return forward(dynamics,originalOffsets);
        if(!compatible||total!=9){Fail();return forward(9,offsets);}
        const std::uint64_t* addresses=nullptr;const std::uint32_t* ranges=nullptr;
        if(!DrawPlan()||!Payload(vertex,addresses,ranges)){Fail();return forward(9,offsets);}
        const auto& plan=sDrawCache->plan;
        if(!plan.Offsets(addresses,ranges,offsets)) {
            // Correct current descriptor atomically through native API; buffers
            // remain current and typed. Rare repair rejects the entire window.
            Fail();UniformWrite repair[4];plan.Writes(vertex,false,repair);
            const auto owner=Read<std::uintptr_t>(sDrawBackend,0x1B4E30);
            if(IsGuestPointer(owner)){
                const auto device=Read<std::uintptr_t>(owner,0x10);
                if(Device(device)){Update(device,4,repair,0,nullptr);++sRow.repairs;}
            }
            return forward(9,offsets);
        }
        if(Owned()&&sActive){
            // Shadow retains ordinary writes/copies and must match them before
            // trusting the normalized form, rather than silently correcting it.
            if(gGalaxyDynamicUniform.mode==2)for(auto delta:offsets)if(delta)Fail();
            ++sRow.vertexBinds;sRow.addressChecks+=9;
        }
        forward(9,offsets);
    }
};
struct Parent {
    bool active;Timing timing;
    UniformDrawCache cache{};
    std::uintptr_t previousBackend=0,previousLayout=0;
    UniformDrawCache* previousCache=nullptr;bool previousChecked=false;
    Parent(std::uintptr_t backend,unsigned kind):active(Owned()),timing(kind) {
        if(!active){sForeign.fetch_add(1,std::memory_order_relaxed);return;}
        if(sActive&&(backend!=gGalaxyDynamicUniform.backend||sDepth))Fail();
        previousBackend=sDrawBackend;previousCache=sDrawCache;previousLayout=sDrawLayout;previousChecked=sDrawLayoutChecked;
        sDrawBackend=backend;sDrawCache=&cache;sDrawLayout=0;sDrawLayoutChecked=false;
        ++sDepth;
    }
    ~Parent(){if(active){--sDepth;sDrawBackend=previousBackend;sDrawCache=previousCache;sDrawLayout=previousLayout;sDrawLayoutChecked=previousChecked;}}
};
HOOK_DEFINE_TRAMPOLINE(UniformIndexed) {
    static void Callback(std::uintptr_t backend,const void* packet,const void* indices){Parent p(backend,0);Orig(backend,packet,indices);}
};
HOOK_DEFINE_TRAMPOLINE(UniformOther) {
    static void Callback(std::uintptr_t backend,const void* packet){if(!packet)return Orig(backend,packet);Parent p(backend,1);Orig(backend,packet);}
};
}
void DynamicUniformBoundary(std::uintptr_t backend) {
    if(!Owned()||!Backend(backend))return;
    auto& b=gGalaxyDynamicUniform;++b.sequence;
    if(!sActive&&b.state!=1&&b.request<1)return;
    const auto now=Clock(),frame=Read<std::uint64_t>(backend,0x60C8);const auto bank=Read<std::uint32_t>(backend,0x60C0);
    if(sActive) {
        sActive=false;sRow.lastFrame=frame;sRow.end=now;
        if(sForeign.load(std::memory_order_relaxed)!=sFirstForeign)sRow.flags|=0x100;
        bool valid=b.backend==backend&&b.thread==Thread()&&bank<2&&!sDepth&&frame==sRow.firstFrame+1&&now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&sRow.flags==b.mode;
        if(valid){const auto stats=backend+0x6020+(bank^1)*0x40;sRow.draws=Read<std::uint32_t>(stats,0x30);valid=sRow.metric[0].calls+sRow.metric[1].calls==sRow.draws&&sRow.vertexBinds&&sRow.addressChecks==9*sRow.vertexBinds&&!sRow.failures&&!sRow.repairs;}
        if(valid)b.rows[b.count++]=sRow;else{++b.rejected;b.shadowPassed=0;if(b.mode==3)b.state=2;}
        if(b.count==240||b.rejected==240){b.state=2;if(b.mode==2&&b.count==240&&!b.rejected)b.shadowPassed=1;}
    }
    if(b.request>=1&&b.request<=3) {
        b.mode=b.request;b.request=0;b.state=1;b.count=b.rejected=0;
        if(b.mode==2)b.shadowPassed=0;
        asm volatile("mrs %0,cntfrq_el0":"=r"(b.frequency));
        if(b.mode==3&&!b.shadowPassed){b.state=2;return;}
    }
    if(b.state!=1||bank>1||b.count>=240)return;
    if((b.backend&&b.backend!=backend)||(b.thread&&b.thread!=Thread())){b.shadowPassed=0;b.state=2;return;}
    b.backend=backend;b.thread=Thread();sRow={};sFirstForeign=sForeign.load(std::memory_order_relaxed);sRow.firstFrame=frame;sRow.start=now;sRow.flags=b.mode;sActive=true;
    if(b.mode!=2){const auto before=Clock();auto& m=sRow.metric[4];m.ticks=Clock()-before;m.maximum=m.ticks;m.calls=1;}
}
void InstallDynamicUniform() {
    UniformPoolCreate::InstallAtOffset(kPoolThunk);UniformLayoutCreate::InstallAtOffset(kLayoutThunk);
    UniformUpdate::InstallAtOffset(kUpdateThunk);UniformBind::InstallAtOffset(kBindThunk);
    UniformIndexed::InstallAtOffset(kIndexed);UniformOther::InstallAtOffset(kOther);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0203 ATT-0183 dynamic uniforms installed default-native shadow-required bounded=240 kill=1");
}
}
#endif
