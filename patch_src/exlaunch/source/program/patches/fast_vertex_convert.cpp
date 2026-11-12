#include "fast_vertex_convert.hpp"
#if defined(GALAXY_FAST_VERTEX_CONVERT) && GALAXY_FAST_VERTEX_CONVERT
#include "producer_cost.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
namespace mod::patches {
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD-0474/0475: the GPU_Decoding producer flush (6DF0 -> A844 -> 49FB8) calls
// one converter per vertex attribute as (descriptor, src, count, srcStride).
// Each writes count vertices to [descriptor+0x18] with output stride u32
// [descriptor+0x44] (reloaded every vertex natively), advances src by the
// zero-extended w3 stride and returns the source attribute size; 49FB8 then
// advances the descriptor itself. Entries follow ret/udf, are referenced only
// from converter tables in .data.2 and no branch targets their first 16 bytes:
//   535468  2 x u16 byte swap  -> 4    (tables 954348, 954648)
//   535760  3 x u32 byte swap  -> 0xC  (tables 9543E8, 9546E8)
//   52F920  2 x u32 byte swap  -> 8    (table 953AD8)
// Native loops move one element per load/store; these replacements write the
// same bytes with one wide load, rev and store per vertex, four vertices per
// iteration (EVD-0476/0477). Replace hooks: the cbz opening is never
// relocated and the original is not called.
constexpr std::uintptr_t kSwap16x2=0x535468,kSwap32x3=0x535760,kSwap32x2=0x52F920;
template<class T>T Load(const std::uint8_t* p){T v;std::memcpy(&v,p,sizeof v);return v;}
template<class T>void Store(std::uint8_t* p,T v){std::memcpy(p,&v,sizeof v);}
std::uint32_t Rev16(std::uint32_t v){std::uint32_t r;asm("rev16 %w0,%w1":"=r"(r):"r"(v));return r;}
std::uint64_t Rev32(std::uint64_t v){std::uint64_t r;asm("rev32 %0,%1":"=r"(r):"r"(v));return r;}
std::uint8_t* Out(std::uintptr_t d){return *reinterpret_cast<std::uint8_t* const*>(d+0x18);}
std::uint32_t OutStride(std::uintptr_t d){return *reinterpret_cast<const std::uint32_t*>(d+0x44);}
// Self-check of the first calls against element-wise native semantics.
enum Kind:unsigned{K16x2,K32x3,K32x2,KindCount};
constexpr const char* kKindName[KindCount]={"swap16x2","swap32x3","swap32x2"};
constexpr unsigned kElems[KindCount]={2,3,2};
constexpr unsigned kVerifyCalls=256;
std::atomic<std::uint32_t> sCalls[KindCount]{},sMismatch[KindCount]{};
// Plain flag read keeps the steady state free of exclusive-monitor atomics.
std::atomic<bool> sVerified[KindCount]{};
__attribute__((noinline,cold)) void VerifySlow(unsigned k,const std::uint8_t* src,const std::uint8_t* out,std::uint32_t n,
                                              std::uint32_t stride,std::uint32_t outStride){
    const auto call=sCalls[k].fetch_add(1,std::memory_order_relaxed);
    if(call>=kVerifyCalls){sVerified[k].store(true,std::memory_order_relaxed);return;}
    bool ok=true;
    for(std::uint32_t i=0;ok&&i<n&&i<16;++i){
        const auto* s=src+static_cast<std::uint64_t>(i)*stride;
        const auto* o=out+static_cast<std::uint64_t>(i)*outStride;
        for(unsigned e=0;ok&&e<kElems[k];++e){
            if(k==K16x2){
                const auto h=Load<std::uint16_t>(s+e*2);
                ok=Load<std::uint16_t>(o+e*2)==static_cast<std::uint16_t>((h>>8)|(h<<8));
            }else ok=Load<std::uint32_t>(o+e*4)==__builtin_bswap32(Load<std::uint32_t>(s+e*4));
        }
    }
    if(!ok)sMismatch[k].fetch_add(1,std::memory_order_relaxed);
    if(call+1==kVerifyCalls){
        sVerified[k].store(true,std::memory_order_relaxed);
        Logging.Log("[GalaxyCoop:smg1] PATCH-0190 fast vertex convert %s verified calls=%u mismatches=%u kill=1",
                    kKindName[k],kVerifyCalls,sMismatch[k].load(std::memory_order_relaxed));
    }
}
inline void Verify(unsigned k,const std::uint8_t* src,const std::uint8_t* out,std::uint32_t n,std::uint32_t stride,std::uint32_t outStride){
    if(!sVerified[k].load(std::memory_order_relaxed))VerifySlow(k,src,out,n,stride,outStride);
}
// Four vertices per iteration through register-offset addressing; T converts
// one vertex from s to o.
template<class T>void Convert(const std::uint8_t* s,std::uint8_t* o,std::uint32_t n,std::uint32_t stride,std::uint32_t outStride,T one){
    const std::uint64_t s1=stride,s2=s1*2,s3=s1*3,s4=s1*4,o1=outStride,o2=o1*2,o3=o1*3,o4=o1*4;
    for(;n>=4;n-=4){one(s,o);one(s+s1,o+o1);one(s+s2,o+o2);one(s+s3,o+o3);s+=s4;o+=o4;}
    for(;n;--n){one(s,o);s+=s1;o+=o1;}
}
#if defined(GALAXY_PRODUCER_COST_DIAG) && GALAXY_PRODUCER_COST_DIAG
// DIAG-0210: time every producer conversion; one in four calls of 16+
// vertices also converts the same source into plain module memory, in
// alternating order, so destination cost can be told from conversion cost.
alignas(64) std::uint8_t sScratch[0x10000];
std::uint32_t sAbCalls=0,sAbPairs=0;
template<class T>void Run(unsigned k,const std::uint8_t* s,std::uint8_t* o,std::uint32_t n,std::uint32_t stride,std::uint32_t outStride,T one){
    if(!ProducerCostActive()){Convert(s,o,n,stride,outStride,one);return;}
    const bool sample=n>=16&&static_cast<std::uint64_t>(n)*outStride+16<=sizeof sScratch&&!(++sAbCalls&3);
    const bool scratchFirst=sample&&(++sAbPairs&1);
    std::uint64_t scratch=0;
    auto toScratch=[&]{
        const auto t=ProducerCostClock();
        Convert(s,sScratch,n,stride,outStride,one);
        scratch=ProducerCostClock()-t;
        asm volatile(""::"r"(sScratch):"memory");
    };
    if(scratchFirst)toScratch();
    const auto t=ProducerCostClock();
    Convert(s,o,n,stride,outStride,one);
    const auto real=ProducerCostClock()-t;
    if(sample&&!scratchFirst)toScratch();
    ProducerCostConvert(k,real,n,sample,scratch);
}
#else
template<class T>inline void Run(unsigned,const std::uint8_t* s,std::uint8_t* o,std::uint32_t n,std::uint32_t stride,std::uint32_t outStride,T one){
    Convert(s,o,n,stride,outStride,one);
}
#endif
HOOK_DEFINE_REPLACE(Swap16x2){
    static std::uint32_t Callback(std::uintptr_t d,const std::uint8_t* src,std::uint32_t n,std::uint32_t stride){
        if(!n)return 4;
        auto* const out=Out(d);const auto outStride=OutStride(d);
        Run(K16x2,src,out,n,stride,outStride,[](const std::uint8_t* s,std::uint8_t* o){Store<std::uint32_t>(o,Rev16(Load<std::uint32_t>(s)));});
        Verify(K16x2,src,out,n,stride,outStride);
        return 4;
    }
};
HOOK_DEFINE_REPLACE(Swap32x3){
    static std::uint32_t Callback(std::uintptr_t d,const std::uint8_t* src,std::uint32_t n,std::uint32_t stride){
        if(!n)return 0xC;
        auto* const out=Out(d);const auto outStride=OutStride(d);
        Run(K32x3,src,out,n,stride,outStride,[](const std::uint8_t* s,std::uint8_t* o){
            Store<std::uint64_t>(o,Rev32(Load<std::uint64_t>(s)));
            Store<std::uint32_t>(o+8,__builtin_bswap32(Load<std::uint32_t>(s+8)));
        });
        Verify(K32x3,src,out,n,stride,outStride);
        return 0xC;
    }
};
HOOK_DEFINE_REPLACE(Swap32x2){
    static std::uint32_t Callback(std::uintptr_t d,const std::uint8_t* src,std::uint32_t n,std::uint32_t stride){
        if(!n)return 8;
        auto* const out=Out(d);const auto outStride=OutStride(d);
        Run(K32x2,src,out,n,stride,outStride,[](const std::uint8_t* s,std::uint8_t* o){Store<std::uint64_t>(o,Rev32(Load<std::uint64_t>(s)));});
        Verify(K32x2,src,out,n,stride,outStride);
        return 8;
    }
};
}
void InstallFastVertexConvert(){
    Swap16x2::InstallAtOffset(kSwap16x2);
    Swap32x3::InstallAtOffset(kSwap32x3);
    Swap32x2::InstallAtOffset(kSwap32x2);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0190 v2 fast vertex convert installed swap16x2=0x535468 swap32x3=0x535760 swap32x2=0x52F920 unroll=4 verify=%u kill=1",kVerifyCalls);
}
}
#endif
