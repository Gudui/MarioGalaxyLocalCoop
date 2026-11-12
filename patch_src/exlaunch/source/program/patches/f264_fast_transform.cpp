#include "f264_fast_transform.hpp"
#if defined(GALAXY_F264_FAST_TRANSFORM) && GALAXY_F264_FAST_TRANSFORM
#include "lib.hpp"
#include "program/loggers.hpp"
#include <arm_neon.h>
#include <atomic>
#include <cstdint>
#include <cstring>
// The native loop rounds every product and sum separately; fused multiply-add
// would change the low bits of the positions.
#pragma GCC optimize("fp-contract=off")
namespace mod::patches {
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// PATCH-0195 / ATT-0199 (EVD-0480/0484): the producer's custom 0xC0 subtype 3
// command (883C -> F264) instances a template of N signed-byte xyz vertices by
// M 3x4 matrices on the CPU and writes M*N float xyz into its GPU allocation.
// Its loop F974..FA24 costs ~30 instructions and two stores per vertex and is
// the hottest part of F264 (EVD-0477: 31 of 59 F264 samples at F9A0); F264
// grows from ~2.5 to ~3.4 ms per split frame when moving (EVD-0480).
// Here F958..F964 becomes an absolute jump to a thunk that runs the same loop
// eight vertices at a time with the native operation order
//   out = (r0*x + r2*z) + (r1*y + r3)   per row, no fused multiply-add,
// so every float is bit-identical, and stores four vertices as three 16-byte
// writes. Loop inputs at F958: x20 command (+0x48 matrices, 0x30 apart),
// x9 template (+8 vertex count, +0x48 attribute table, 0x28 apart), x22 table
// (+0x24 attribute index), x19 -> matrix count, [sp+0x38] output; v0 = 1.0.
// The loop leaves only x8 = [x19] and v0 live at its exit FA48. Nothing
// references F958..F967 and FA48 is entered only from F954 and FA28
// (GalaxyCodeRefs, ghidra_f264_loops_20261013). Install verifies the four
// original words first.
constexpr bool kF264FastTransformEnabled=true;
constexpr std::uintptr_t kLoopStart=0xF958,kLoopExit=0xFA48;
constexpr std::uint32_t kOriginal[4]={0xF9401FF0,0xAA1F03EA,0x9101228B,0x9101212C};
constexpr unsigned kVerifyCalls=128;
std::atomic<std::uint32_t> sCalls{0},sMismatch{0},sVertices{0};
float Row(const float* r,float x,float y,float z){return (r[0]*x+r[2]*z)+(r[1]*y+r[3]);}
void Scalar(const float* m,const std::int8_t* s,std::uint8_t* o){
    const float x=s[0],y=s[1],z=s[2];
    const float v[3]={Row(m,x,y,z),Row(m+4,x,y,z),Row(m+8,x,y,z)};
    std::memcpy(o,v,sizeof v);
}
struct Rows{float32x4_t a,b,c,d;};
Rows Load(const float* r){return {vdupq_n_f32(r[0]),vdupq_n_f32(r[1]),vdupq_n_f32(r[2]),vdupq_n_f32(r[3])};}
float32x4_t Apply(const Rows& r,float32x4_t x,float32x4_t y,float32x4_t z){
    return vaddq_f32(vaddq_f32(vmulq_f32(r.a,x),vmulq_f32(r.c,z)),vaddq_f32(vmulq_f32(r.b,y),r.d));
}
// Interleave four x, y, z lanes into 48 bytes of xyz records.
void Store4(std::uint8_t* o,float32x4_t x,float32x4_t y,float32x4_t z){
    static const std::uint8_t kIdx[3][16]={
        {0,1,2,3,16,17,18,19,32,33,34,35,4,5,6,7},
        {20,21,22,23,36,37,38,39,8,9,10,11,24,25,26,27},
        {40,41,42,43,12,13,14,15,28,29,30,31,44,45,46,47}};
    const uint8x16x3_t t={{vreinterpretq_u8_f32(x),vreinterpretq_u8_f32(y),vreinterpretq_u8_f32(z)}};
    vst1q_u8(o,vqtbl3q_u8(t,vld1q_u8(kIdx[0])));
    vst1q_u8(o+16,vqtbl3q_u8(t,vld1q_u8(kIdx[1])));
    vst1q_u8(o+32,vqtbl3q_u8(t,vld1q_u8(kIdx[2])));
}
void Transform(const float* m,const std::int8_t* s,std::uint32_t n,std::uint8_t* o){
    const Rows r0=Load(m),r1=Load(m+4),r2=Load(m+8);
    std::uint32_t v=0;
    for(;v+8<=n;v+=8,s+=24,o+=96){
        const int8x8x3_t in=vld3_s8(s);
        const int16x8_t x=vmovl_s8(in.val[0]),y=vmovl_s8(in.val[1]),z=vmovl_s8(in.val[2]);
        const float32x4_t xl=vcvtq_f32_s32(vmovl_s16(vget_low_s16(x))),xh=vcvtq_f32_s32(vmovl_s16(vget_high_s16(x)));
        const float32x4_t yl=vcvtq_f32_s32(vmovl_s16(vget_low_s16(y))),yh=vcvtq_f32_s32(vmovl_s16(vget_high_s16(y)));
        const float32x4_t zl=vcvtq_f32_s32(vmovl_s16(vget_low_s16(z))),zh=vcvtq_f32_s32(vmovl_s16(vget_high_s16(z)));
        Store4(o,Apply(r0,xl,yl,zl),Apply(r1,xl,yl,zl),Apply(r2,xl,yl,zl));
        Store4(o+48,Apply(r0,xh,yh,zh),Apply(r1,xh,yh,zh),Apply(r2,xh,yh,zh));
    }
    for(;v<n;++v,s+=3,o+=12)Scalar(m,s,o);
}
// First calls: the vector path into a local buffer against the scalar
// native-order reference, so the GPU allocation is never read back.
__attribute__((noinline,cold)) void Verify(const float* m,const std::int8_t* s,std::uint32_t n){
    const auto call=sCalls.fetch_add(1,std::memory_order_relaxed);
    if(call>=kVerifyCalls)return;
    const std::uint32_t k=n<16?n:16;
    std::uint8_t fast[16*12],ref[16*12];
    Transform(m,s,k,fast);
    for(std::uint32_t v=0;v<k;++v)Scalar(m,s+v*3,ref+v*12);
    if(std::memcmp(fast,ref,k*12))sMismatch.fetch_add(1,std::memory_order_relaxed);
    sVertices.fetch_add(n,std::memory_order_relaxed);
    if(call+1==kVerifyCalls)
        Logging.Log("[GalaxyCoop:smg1] PATCH-0195 f264 fast transform verified calls=%u mismatches=%u vertices=%u kill=1",
            kVerifyCalls,sMismatch.load(std::memory_order_relaxed),sVertices.load(std::memory_order_relaxed));
}
}
extern "C" {
std::uintptr_t gGalaxyF264Return=0;
void GalaxyF264PositionThunk();
// Mirrors F958..FA24: at least one matrix, the vertex count and source are
// re-read per matrix, the output advances by count*12; returns [x19].
std::uint64_t GalaxyF264Positions(std::uintptr_t command,std::uintptr_t templ,std::uintptr_t table,std::uint8_t* out,
                                  const std::uint32_t* matrices){
    std::uint32_t m=0;
    do{
        const auto n=*reinterpret_cast<const std::uint32_t*>(templ+8);
        if(n){
            const auto index=*reinterpret_cast<const std::uint32_t*>(table+0x24);
            const auto* src=*reinterpret_cast<const std::int8_t* const*>(templ+0x48+static_cast<std::uint64_t>(index)*0x28);
            const auto* mtx=reinterpret_cast<const float*>(command+0x48+static_cast<std::uint64_t>(m)*0x30);
            if(sCalls.load(std::memory_order_relaxed)<kVerifyCalls)Verify(mtx,src,n);
            Transform(mtx,src,n,out);
            out+=static_cast<std::uint64_t>(n)*12;
        }
        ++m;
    }while(m<*matrices);
    return *matrices;
}
}
// Entered by the absolute jump at F958 with F264's registers live; F264's
// frame is 16-byte aligned, so [sp+0x38] is [our sp+0x68].
asm(".pushsection .text\n.balign 4\n.global GalaxyF264PositionThunk\n.type GalaxyF264PositionThunk, %function\n"
    "GalaxyF264PositionThunk:\n"
    "sub sp,sp,#0x30\nstp x29,x30,[sp]\nstr q0,[sp,#0x10]\n"
    "mov x0,x20\nmov x1,x9\nmov x2,x22\nldr x3,[sp,#0x68]\nmov x4,x19\n"
    "bl GalaxyF264Positions\n"
    "mov x8,x0\nmov x10,x0\n"
    "ldr q0,[sp,#0x10]\nldp x29,x30,[sp]\nadd sp,sp,#0x30\n"
    "adrp x17,gGalaxyF264Return\nldr x17,[x17,:lo12:gGalaxyF264Return]\nbr x17\n"
    ".size GalaxyF264PositionThunk, .-GalaxyF264PositionThunk\n.popsection\n");
void InstallF264FastTransform(){
    if(!kF264FastTransformEnabled)return;
    const auto base=exl::util::modules::GetTargetStart();
    const auto* code=reinterpret_cast<const std::uint32_t*>(base+kLoopStart);
    for(unsigned i=0;i<4;++i)if(code[i]!=kOriginal[i]){
        Logging.Log("[GalaxyCoop:smg1] PATCH-0195 f264 fast transform refused word%u=0x%08x kill=1",i,code[i]);
        return;
    }
    gGalaxyF264Return=base+kLoopExit;
    {
        exl::patch::CodePatcher p(kLoopStart);
        p.Write<std::uint32_t>(0x58000051);  // ldr x17, #8
        p.Write<std::uint32_t>(0xD61F0220);  // br x17
        p.Write<std::uint64_t>(reinterpret_cast<std::uint64_t>(&GalaxyF264PositionThunk));
    }
    Logging.Log("[GalaxyCoop:smg1] PATCH-0195 f264 fast transform installed loop=0xF958 exit=0xFA48 width=8 verify=%u kill=1",kVerifyCalls);
}
}
#endif
