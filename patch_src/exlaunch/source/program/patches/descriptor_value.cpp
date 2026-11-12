#include "descriptor_value.hpp"
#if defined(GALAXY_DESCRIPTOR_VALUE_EXPERIMENT) && GALAXY_DESCRIPTOR_VALUE_EXPERIMENT
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Descriptor value experiment requires production owner
#endif
#if (defined(GALAXY_DRAW_ASSOCIATION_DIAG) && GALAXY_DRAW_ASSOCIATION_DIAG) || (defined(GALAXY_DYNAMIC_UNIFORM_EXPERIMENT) && GALAXY_DYNAMIC_UNIFORM_EXPERIMENT) || (defined(GALAXY_BINDING_TIMING_DIAG) && GALAXY_BINDING_TIMING_DIAG) || (defined(GALAXY_VERTEX_BATCH_DIAG) && GALAXY_VERTEX_BATCH_DIAG) || (defined(GALAXY_DRAW_COMMAND_TIMING_DIAG) && GALAXY_DRAW_COMMAND_TIMING_DIAG)
#error Fulfilled/retired overlapping hooks must be omitted
#endif
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cstring>
#include <algorithm>
#include <atomic>
namespace mod::patches {
extern "C" { DescriptorValueBlock gGalaxyDescriptorValue{}; }
extern "C" void GalaxyValueBindImport(std::uintptr_t,unsigned,std::uintptr_t,unsigned,unsigned,const std::uintptr_t*,unsigned,const unsigned*,std::uintptr_t);
extern "C" void GalaxyValueUpdateImport(std::uintptr_t,unsigned,const void*,unsigned,const void*,std::uintptr_t);
extern "C" void GalaxyValueDrawImport(std::uintptr_t,unsigned,unsigned,unsigned,int,unsigned,std::uintptr_t);
// EVD0365/0401/0425: full aligned16B imports; GOT untouched, including lazy PLT0 x16.
asm(".pushsection .text\n.balign 4\n.global GalaxyValueBindImport\nGalaxyValueBindImport:\nldr x16,[sp]\nldr x17,[x16]\nbr x17\n"
    ".balign 4\n.global GalaxyValueUpdateImport\nGalaxyValueUpdateImport:\nmov x16,x5\nldr x17,[x16]\nbr x17\n"
    ".balign 4\n.global GalaxyValueDrawImport\nGalaxyValueDrawImport:\nmov x16,x6\nldr x17,[x16]\nbr x17\n.popsection\n");
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938;
// subsdk4 SHA9d7b958f58dcfbe013b3dbd0f126096a6b85452249f09fb7841247e348c2f46d.
// EVD0343/0345/0373/0401/0422/0440/0444: typed9type6 payloads; true draw/submit
// entries and fence-owned native resources. No guessed lease from ring indices.
constexpr std::uintptr_t kIndexed=0x639C0,kOther=0x243A0,kSubmit=0x7B460;
constexpr std::uintptr_t kBind=0x5F9F40,kBindGot=0x919AE0,kUpdate=0x5F9F30,kUpdateGot=0x919AD8,kDraw=0x5F9F10,kDrawGot=0x919AC8;
struct Payload {
    std::uintptr_t descriptor=0,layout=0,storage[9]{};
    std::uint64_t gpu[9]{},offset[9]{},capacity[9]{},size[9]{};
};
struct Previous { bool valid=false;std::uint64_t epoch=0;std::uintptr_t backend=0,command=0,pipelineLayout=0;unsigned bank=0;Payload payload{}; };
Previous sPrevious{},sPending{}; // Only within one current recording epoch; never followed after invalidation.
DescriptorValueRow sRow{};
bool sActive=false;unsigned sDepth=0;std::uintptr_t sBackend=0;
std::atomic<std::uint64_t> sEpoch{1},sForeign{0};std::uint64_t sFirstForeign=0;
std::uintptr_t Main(){return exl::util::modules::GetTargetStart();}
std::uintptr_t Thread(){std::uintptr_t v;asm volatile("mrs %0,tpidrro_el0":"=r"(v));return v;}
std::uint64_t Clock(){std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
template<class T>T Read(std::uintptr_t p,std::uintptr_t off=0){return *reinterpret_cast<const T*>(p+off);}
bool Span(std::uintptr_t p,std::size_t size){return p>=kMinGuestPointer&&p<kMaxGuestPointer&&!(p&3)&&size<=kMaxGuestPointer-p;}
bool Owned(){return gGalaxyPushFilter.thread&&Thread()==gGalaxyPushFilter.thread;}
bool Backend(std::uintptr_t p){return IsGuestPointer(p)&&Read<std::uintptr_t>(p)==Main()+0x889C68;}
void Fail(){++sRow.failures;sRow.flags|=0x100;gGalaxyDescriptorValue.shadowPassed=0;sPrevious.valid=sPending.valid=false;}
struct Timing {
    unsigned kind;bool active;std::uint64_t start;
    explicit Timing(unsigned k):kind(k),active(Owned()&&sActive&&gGalaxyDescriptorValue.mode!=2),start(active?Clock():0){}
    ~Timing(){if(Owned()&&sActive){auto& m=sRow.metric[kind];const auto d=active?Clock()-start:0;m.ticks+=d;m.maximum=std::max(m.maximum,d);++m.calls;}}
};
bool Command(std::uintptr_t backend,std::uintptr_t command,unsigned& bank){
    if(!Backend(backend))return false;
    bank=Read<unsigned>(backend,0x1061F8);
    return bank<12&&Read<unsigned>(backend,0x128C750+bank*4)==1&&command==Read<std::uintptr_t>(backend,0x106190+bank*8);
}
bool GetPayload(std::uintptr_t backend,std::uintptr_t descriptor,Payload& out,bool current){
    const auto thunk=Read<std::uintptr_t>(Main()+kBindGot);
    if(!IsGuestPointer(thunk)||thunk<0x1C92310)return false;
    const auto native=thunk-0x1C92310,root=backend+0x1886A0;
    const auto lh=Read<std::uintptr_t>(root,0x18);
    if(!IsGuestPointer(lh)||lh<0x50||!IsGuestPointer(descriptor)||descriptor<0x50)return false;
    const auto layout=lh-0x50;
    if(Read<std::uintptr_t>(layout)!=native+0x262B888||Read<unsigned>(layout,0x10C)!=4||
       Read<std::uintptr_t>(descriptor-0x50)!=native+0x262B868||Read<std::uintptr_t>(descriptor-0x50,0x108)!=layout)return false;
    const auto bindings=Read<std::uintptr_t>(layout,0x118),addresses=Read<std::uintptr_t>(descriptor-0x50,0x138),ranges=Read<std::uintptr_t>(descriptor-0x50,0x140);
    if(!Span(bindings,4*0x40)||!Span(addresses,9*8)||!Span(ranges,9*4))return false;
    constexpr unsigned counts[4]={4,2,2,1};unsigned start=0;
    for(unsigned i=0;i<4;++i){const auto p=bindings+i*0x40;
        if(Read<unsigned>(p)!=6||Read<unsigned>(p,4)!=counts[i]||Read<unsigned>(p,0xC)!=1||Read<unsigned>(p,0x24)!=start)return false;
        start+=counts[i];
    }
    if(current){const auto cursor=Read<unsigned>(root,0x8108);
        if(cursor>=4096||descriptor!=Read<std::uintptr_t>(root,0x28+cursor*8))return false;
    }
    out.descriptor=descriptor;out.layout=layout;
    for(unsigned i=0;i<9;++i){
        const auto t=Main()+0xB9FA1710+0x1A8+i*0x80,buffer=Read<std::uintptr_t>(t);
        const auto expected=i<6?256ULL:i<8?512ULL:768ULL;
        if(!IsGuestPointer(buffer)||buffer<0x50||Read<std::uintptr_t>(buffer-0x50)!=native+0x262B948)return false;
        const auto gpu=Read<std::uint64_t>(buffer,0xA8),capacity=Read<std::uint64_t>(buffer,0xB0),storage=Read<std::uintptr_t>(t,0x28),address=Read<std::uint64_t>(addresses,i*8);
        if(!gpu||gpu>UINT64_MAX-capacity||capacity!=expected*4096||!Span(storage,capacity)||address<gpu||address-gpu>capacity-expected||
           (address-gpu)%expected||Read<unsigned>(ranges,i*4)!=expected)return false;
        const auto offset=address-gpu;
        if(current&&(Read<std::uintptr_t>(root,0x8030+i*24)!=buffer||Read<std::uint64_t>(root,0x8038+i*24)!=offset||Read<std::uint64_t>(root,0x8040+i*24)!=expected))return false;
        out.gpu[i]=gpu;out.capacity[i]=capacity;out.storage[i]=storage;out.offset[i]=offset;out.size[i]=expected;
    }
    return true;
}
bool Stable(const Payload& a,const Payload& b){
    if(a.descriptor!=b.descriptor||a.layout!=b.layout)return false;
    for(unsigned i=0;i<9;++i)if(a.storage[i]!=b.storage[i]||a.gpu[i]!=b.gpu[i]||a.offset[i]!=b.offset[i]||a.size[i]!=b.size[i]||a.capacity[i]!=b.capacity[i])return false;
    return true;
}
bool Equal(const Payload& a,const Payload& b){
    if(a.layout!=b.layout)return false;
    Timing timing(4);++sRow.oracleChecks;
    for(unsigned i=0;i<9;++i){
        if(a.storage[i]!=b.storage[i]||a.gpu[i]!=b.gpu[i]||a.size[i]!=b.size[i]||a.capacity[i]!=b.capacity[i])return false;
        if(a.offset[i]!=b.offset[i]){
            sRow.bytes+=a.size[i];
            if(std::memcmp(reinterpret_cast<const void*>(a.storage[i]+a.offset[i]),reinterpret_cast<const void*>(b.storage[i]+b.offset[i]),a.size[i])!=0)return false;
        }
    }
    return true;
}
HOOK_DEFINE_REPLACE(ValueBind){
    static void Callback(std::uintptr_t command,unsigned point,std::uintptr_t pipelineLayout,unsigned first,unsigned count,const std::uintptr_t* sets,unsigned dynamics,const unsigned* offsets){
        Timing timing(3);
        auto forward=[&](const std::uintptr_t* selected){GalaxyValueBindImport(command,point,pipelineLayout,first,count,selected,dynamics,offsets,Main()+kBindGot);};
        if(!Owned()){sEpoch.fetch_add(1,std::memory_order_relaxed);return forward(sets);}
        sPending.valid=false;
        if(!sActive||sDepth!=1||gGalaxyDescriptorValue.mode==1)return forward(sets);
        if(point||dynamics||first>3||count==0||count>4-first||first+count!=4||!Span(reinterpret_cast<std::uintptr_t>(sets),count*8))return forward(sets);
        const auto slot=3-first;unsigned bank=0;
        if(!Command(sBackend,command,bank)){Fail();return forward(sets);}
        Payload now{};if(!GetPayload(sBackend,sets[slot],now,true)){Fail();return forward(sets);}
        // All sets/layout/dynamic arguments preserved. Only native type6 vertex set3 qualifies.
        const auto thunk=Read<std::uintptr_t>(Main()+kBindGot),native=thunk-0x1C92310;
        if(!IsGuestPointer(pipelineLayout)||pipelineLayout<0x50||Read<std::uintptr_t>(pipelineLayout-0x50)!=native+0x262B8A8||Read<std::uintptr_t>(pipelineLayout-0x50,0x100+3*8)!=now.layout){Fail();return forward(sets);}
        ++sRow.vertexBinds;
        const auto epoch=sEpoch.load(std::memory_order_relaxed);
        Previous selected{true,epoch,sBackend,command,pipelineLayout,bank,now};
        if(sPrevious.valid&&sPrevious.epoch==epoch&&sPrevious.backend==sBackend&&sPrevious.command==command&&sPrevious.bank==bank&&sPrevious.pipelineLayout==pipelineLayout&&sPrevious.payload.descriptor!=now.descriptor){
            Payload old{};
            // Only a previously completed native draw in THIS recording epoch.
            // Re-read old descriptor arrays; pool overwrite/class/answer change refuses.
            if(!GetPayload(sBackend,sPrevious.payload.descriptor,old,false)||!Stable(old,sPrevious.payload)){Fail();return forward(sets);}
            if(Equal(old,now)){
                ++sRow.candidates;
                if(gGalaxyDescriptorValue.mode==3&&gGalaxyDescriptorValue.shadowPassed){selected=sPrevious;++sRow.selected;}
            }
        }
        std::uintptr_t adapted[4]{};for(unsigned i=0;i<count;++i)adapted[i]=sets[i];adapted[slot]=selected.payload.descriptor;
        if(sEpoch.load(std::memory_order_relaxed)!=epoch){sPrevious.valid=false;return forward(sets);}
        forward(adapted);sPending=selected;
    }
};
HOOK_DEFINE_REPLACE(ValueUpdate){
    static void Callback(std::uintptr_t device,unsigned count,const void* writes,unsigned copies,const void* copy){
        Timing timing(2);GalaxyValueUpdateImport(device,count,writes,copies,copy,Main()+kUpdateGot);
    }
};
HOOK_DEFINE_REPLACE(ValueDraw){
    static void Callback(std::uintptr_t command,unsigned indices,unsigned instances,unsigned firstIndex,int vertexOffset,unsigned firstInstance){
        GalaxyValueDrawImport(command,indices,instances,firstIndex,vertexOffset,firstInstance,Main()+kDrawGot);
        if(!Owned()){sForeign.fetch_add(1,std::memory_order_relaxed);sEpoch.fetch_add(1,std::memory_order_relaxed);return;}
        if(!sActive)return;
        ++sRow.drawCalls;
        unsigned bank=0;if(sDepth!=1||!Command(sBackend,command,bank)){Fail();return;}
        if(sPending.valid&&sPending.epoch==sEpoch.load(std::memory_order_relaxed)&&sPending.command==command&&sPending.bank==bank){sPrevious=sPending;sPending.valid=false;}
    }
};
struct Parent{
    bool active;Timing timing;
    Parent(std::uintptr_t backend,unsigned kind):active(Owned()&&sActive),timing(kind){
        if(active){if(sDepth||!Backend(backend)||backend!=gGalaxyDescriptorValue.backend){Fail();active=false;return;}sDepth=1;sBackend=backend;sPending.valid=false;}
    }
    ~Parent(){if(active){sDepth=0;sBackend=0;sPending.valid=false;}}
};
HOOK_DEFINE_TRAMPOLINE(ValueIndexed){static void Callback(std::uintptr_t backend,const void* packet,const void* attributes){Parent p(backend,0);Orig(backend,packet,attributes);}};
HOOK_DEFINE_TRAMPOLINE(ValueOther){static void Callback(std::uintptr_t backend,const void* packet){Parent p(backend,1);Orig(backend,packet);}};
HOOK_DEFINE_TRAMPOLINE(ValueSubmit){static void Callback(std::uintptr_t backend,unsigned flags){DescriptorValueInvalidate();if(Owned()&&sActive)++sRow.submissions;Orig(backend,flags);}};
}
void DescriptorValueInvalidate(){sEpoch.fetch_add(1,std::memory_order_relaxed);if(Owned())sPrevious.valid=sPending.valid=false;}
void DescriptorValueBoundary(std::uintptr_t backend){
    DescriptorValueInvalidate();if(!Owned()||!Backend(backend))return;
    auto& b=gGalaxyDescriptorValue;++b.sequence;
    if(!sActive&&b.state!=1&&!b.request)return;
    const auto now=Clock(),frame=Read<std::uint64_t>(backend,0x60C8);const auto bank=Read<unsigned>(backend,0x60C0);
    if(sActive){sActive=false;sRow.lastFrame=frame;sRow.end=now;
        if(sForeign.load(std::memory_order_relaxed)!=sFirstForeign)sRow.flags|=0x200;
        bool valid=b.backend==backend&&b.thread==Thread()&&bank<2&&!sDepth&&frame==sRow.firstFrame+1&&now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&sRow.flags==b.mode;
        if(valid){sRow.draws=Read<unsigned>(backend,0x6020+(bank^1)*0x40+0x30);valid=sRow.draws&&sRow.metric[0].calls+sRow.metric[1].calls==sRow.draws&&sRow.drawCalls==sRow.draws&&!sRow.failures;}
        if(valid)b.rows[b.count++]=sRow;else{++b.rejected;b.shadowPassed=0;}
        if(b.count==240||b.rejected){b.state=2;if(b.mode==2&&b.count==240&&!b.rejected){std::uint64_t candidates=0;for(const auto& row:b.rows)candidates+=row.candidates;if(candidates)b.shadowPassed=1;}}
    }
    if(b.request>=1&&b.request<=3){b.mode=b.request;b.request=0;b.state=1;b.count=b.rejected=0;if(b.mode==2)b.shadowPassed=0;
        asm volatile("mrs %0,cntfrq_el0":"=r"(b.frequency));
        if(b.mode==3&&!b.shadowPassed){b.state=2;return;}
    }
    if(b.state!=1||bank>1||b.count>=240)return;
    if((b.backend&&b.backend!=backend)||(b.thread&&b.thread!=Thread())){b.shadowPassed=0;b.state=2;return;}
    b.backend=backend;b.thread=Thread();sRow={};sFirstForeign=sForeign.load(std::memory_order_relaxed);sRow.firstFrame=frame;sRow.start=now;sRow.flags=b.mode;sActive=true;
    if(b.mode!=2){const auto before=Clock();auto& m=sRow.metric[5];m.ticks=Clock()-before;m.maximum=m.ticks;m.calls=1;}
}
void InstallDescriptorValue(){
    ValueIndexed::InstallAtOffset(kIndexed);ValueOther::InstallAtOffset(kOther);ValueSubmit::InstallAtOffset(kSubmit);
    ValueBind::InstallAtOffset(kBind);ValueUpdate::InstallAtOffset(kUpdate);ValueDraw::InstallAtOffset(kDraw);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0205 ATT-0184 descriptor values installed default-native shadow-required bounded=240 kill=1");
}
}
#endif
