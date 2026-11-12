#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>

namespace mod::patches {
// ATT0183/EVD0425/426. Pure address/range planner; no guest pointers cached.
struct UniformBufferInfo { std::uint64_t buffer=0,offset=0,range=0; };
struct UniformBinding { std::uint32_t binding=0,type=0,count=0,stages=0;std::uint64_t samplers=0; };
struct UniformWrite {
    std::uint32_t type=35,pad=0;std::uint64_t next=0,set=0;
    std::uint32_t binding=0,element=0,count=0,descriptorType=8;
    const void* images=nullptr;const UniformBufferInfo* buffers=nullptr;const void* texels=nullptr;
};
struct UniformCopy {
    std::uint32_t type=36,pad=0;std::uint64_t next=0,source=0;
    std::uint32_t sourceBinding=0,sourceElement=0;std::uint64_t destination=0;
    std::uint32_t destinationBinding=0,destinationElement=0,count=0,pad2=0;
};
static_assert(sizeof(UniformBufferInfo)==24&&sizeof(UniformBinding)==24);
static_assert(sizeof(UniformWrite)==64&&offsetof(UniformWrite,buffers)==48);
static_assert(sizeof(UniformCopy)==56&&offsetof(UniformCopy,destination)==32);
struct UniformPlan {
    static constexpr unsigned Count=9;
    UniformBufferInfo selected[Count]{},normalized[Count]{};
    std::uint64_t base[Count]{},capacity[Count]{};
    static constexpr std::uint32_t Range(unsigned i) { return i<6?256:i<8?512:768; }
    bool Valid() const {
        for(unsigned i=0;i<Count;++i) {
            const auto& s=selected[i];const auto range=Range(i);
            if(!s.buffer||s.range!=range||capacity[i]!=std::uint64_t(range)*4096||
               s.offset%256||s.offset>capacity[i]-range||!base[i]||
               base[i]>std::numeric_limits<std::uint64_t>::max()-capacity[i]) return false;
        }
        return true;
    }
    bool Normalize() {
        if(!Valid())return false;
        for(unsigned i=0;i<Count;++i)normalized[i]={selected[i].buffer,0,selected[i].range};
        return true;
    }
    // Mixed ordinary/normalized descriptors remain valid after control changes:
    // copies retain either base or the unchanged slot; changed entries get a
    // native update. Derive each delta from the real DS payload, never history.
    bool Offsets(const std::uint64_t* addresses,const std::uint32_t* ranges,std::uint32_t* result) const {
        if(!Valid()||!addresses||!ranges||!result)return false;
        std::uint32_t proposed[Count]{};
        for(unsigned i=0;i<Count;++i) {
            const auto expected=base[i]+selected[i].offset;
            if(ranges[i]!=selected[i].range||addresses[i]<base[i]||addresses[i]>expected)return false;
            const auto delta=expected-addresses[i];
            if(delta%256||delta>std::numeric_limits<std::uint32_t>::max())return false;
            proposed[i]=static_cast<std::uint32_t>(delta);
        }
        for(unsigned i=0;i<Count;++i)result[i]=proposed[i];
        return true;
    }
    bool Stable(const std::uint64_t* addresses,const std::uint32_t* ranges) const {
        if(!Valid()||!addresses||!ranges)return false;
        for(unsigned i=0;i<Count;++i)if(addresses[i]!=base[i]||ranges[i]!=selected[i].range)return false;
        return true;
    }
    void Writes(std::uint64_t set,bool normalizedOffsets,UniformWrite (&writes)[4]) const {
        const unsigned counts[4]={4,2,2,1};unsigned start=0;
        for(unsigned i=0;i<4;++i) {
            writes[i]={};writes[i].set=set;writes[i].binding=i;writes[i].count=counts[i];
            writes[i].buffers=(normalizedOffsets?normalized:selected)+start;start+=counts[i];
        }
    }
};
struct UniformUpdatePlan {
    UniformWrite writes[20]{};UniformCopy copies[16]{};
    unsigned writeCount=0,copyCount=0,removedWrites=0,removedCopies=0;
    bool initialized=false;
    bool Build(unsigned nw,const UniformWrite* inputWrites,const bool* vertexWrites,
               unsigned nc,const UniformCopy* inputCopies,const bool* vertexCopies,
               std::uint64_t vertex,const UniformPlan* normalized,bool alreadyStable) {
        if(nw>16||nc>16||(nw&&(!inputWrites||!vertexWrites))||(nc&&(!inputCopies||!vertexCopies))||
           (normalized&&(!vertex||!normalized->Valid())))return false;
        for(unsigned i=0;i<nw;++i) {
            auto w=inputWrites[i];
            if(vertexWrites[i]) {
                if(w.descriptorType!=6&&w.descriptorType!=8)return false;
                w.descriptorType=8;
                if(normalized&&w.set!=vertex)return false;
            }
            if(normalized&&vertexWrites[i])++removedWrites;else writes[writeCount++]=w;
        }
        for(unsigned i=0;i<nc;++i) {
            if(normalized&&vertexCopies[i]) {
                if(inputCopies[i].destination!=vertex)return false;
                ++removedCopies;
            }else copies[copyCount++]=inputCopies[i];
        }
        if(normalized&&!alreadyStable) {
            UniformWrite initialize[4];normalized->Writes(vertex,true,initialize);
            for(const auto& w:initialize)writes[writeCount++]=w;
            initialized=true;
        }
        return true;
    }
};
// Owned by one Parent stack frame. No cached handle survives a draw return.
struct UniformDrawCache {
    UniformPlan plan{};
    bool ready=false;
    void Invalidate() { ready=false; }
    template<class Reader> bool Get(Reader&& reader) {
        if(ready)return true;
        ready=reader(plan);
        return ready;
    }
};
}
