#include "patch_camera.hpp"
#include "patch_shared.hpp"
#include "patch_actions.hpp"
#include "patch_respawn.hpp"
#include "patch_diagnostics.hpp"
#include "patch_splitscreen.hpp"
#include "planet_camera_math.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace mod::patches {
namespace {
// PATCH-0164 / EVD-0196, SMG1 main SHA-256
// 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// NSO entries/layout rechecked in EVD-0181, EVD-0060. Never change target
// selection at spawn or outside the ordinary CameraManGame calculation.
constexpr std::uintptr_t kGetPosition = 0x03DCC8;
constexpr std::uintptr_t kCameraCalc = 0x0B8A70;
constexpr std::uintptr_t kCameraVtable = 0x81E4C0;
constexpr std::uintptr_t kCameraPosition = 0x494;
using planet::Vec;
using planet::Dot;
using planet::Length;
using planet::Cross;
using planet::Finite;
Vec Read(std::uintptr_t p) { const auto* v=reinterpret_cast<const float*>(p); return {v[0],v[1],v[2]}; }
void Write(std::uintptr_t p, Vec v) { auto* out=reinterpret_cast<float*>(p); out[0]=v.x;out[1]=v.y;out[2]=v.z; }
Vec sCenter{};
bool sHaveCenter=false;
bool sInsideSharedCalc=false;
float sExtraDistance=0;
unsigned sFitLogs=0;
int sLastBin=-1;

bool Participating(void* actor, DeathPhase phase) {
    return IsValidMarioActor(actor) && phase==DeathPhase::Alive && !IsCoopTravelHidden(actor);
}
const float* ActorCamera(void* actor) {
    return reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(actor)+kCameraPosition);
}
// PATCH-0168 / EVD-0201: SMG1 main hash above. Native camera Follow
// 1680D8 calls map-line query1686C0(start,delta,0,null,null). Its keeper
// adapter39C80 returns the hit count. All addresses here are NSO offsets.
// Scene rootAA456178 ->48 ->C8 ->20 ->8 ->18 ->0 is proven by3D50,
// 3D64,3D48,1686C0. Validate every hop live; no cached scene pointers.
constexpr std::uintptr_t kCollisionSceneRoot=0xAA456178;
constexpr std::uintptr_t kMapLineQuery=0x39C80;
constexpr unsigned kVisibilitySampleInterval=6;
bool sOrbitActive=false;
Vec sOrbitUp{},sOrbitTangent{},sOrbitOut{},sOrbitDesired{},sOrbitWatch{};
unsigned sOrbitTicks=0,sOrbitLogs=0;
int sCandidateVisibility=3;

std::uintptr_t Pointer(std::uintptr_t owner,std::uintptr_t offset) {
    if (!IsGuestPointer(owner)) return 0;
    const auto value=*reinterpret_cast<const std::uintptr_t*>(owner+offset);
    return IsGuestPointer(value)?value:0;
}
bool PlayerUp(void* actor,Vec& up) {
    if (!IsValidMarioActor(actor)) return false;
    const auto core=Pointer(reinterpret_cast<std::uintptr_t>(actor),0x3C0);
    if (!core || *reinterpret_cast<void* const*>(core+8)!=actor) return false;
    // A0D48 returns Mario+1E8;11A2E4 negates it for recovery up.
    const Vec gravity=Read(core+0x1E8);
    if (!Finite(gravity)||Length(gravity)<0.001f) return false;
    up=planet::Unit(gravity*-1);
    return true;
}
std::uintptr_t MapKeeper() {
    auto p=Pointer(exl::util::modules::GetTargetStart()+kCollisionSceneRoot,0);
    p=Pointer(p,0x48);p=Pointer(p,0xC8);p=Pointer(p,0x20);
    p=Pointer(p,8);p=Pointer(p,0x18);p=Pointer(p,0);
    if (!p || !Pointer(p,0x18)) return 0;
    const int groups=*reinterpret_cast<const int*>(p+0x128);
    return groups>0 && groups<=32 ? p : 0;
}
bool ClearLine(std::uintptr_t keeper,Vec eye,Vec point) {
    const Vec delta=point-eye;
    using Query=int (*)(std::uintptr_t,const Vec*,const Vec*,int,const void*,const void*);
    const auto query=reinterpret_cast<Query>(exl::util::modules::GetTargetStart()+kMapLineQuery);
    return query(keeper,&eye,&delta,0,nullptr,nullptr)==0;
}
int VisiblePlayers(std::uintptr_t keeper,Vec eye,const Vec points[2],const Vec ups[2]) {
    int mask=0;
    for(int i=0;i<2;++i) {
        // Two body heights prevent a low ledge alone from counting as blind.
        if (ClearLine(keeper,eye,points[i]+ups[i]*40) ||
            ClearLine(keeper,eye,points[i]+ups[i]*120)) mask|=1<<i;
    }
    return mask;
}

// PATCH-0169 / EVD-0203, same SMG1 main hash as the offset table above.
// B8C60 -> C01DC -> C01E4 proves man18 -> director68 -> holder8 target.
// Target constructor10187C allocates78 bytes with vptr81BAA0. Movement70C38
// refreshes actor-local basis20..58 and native player state60..72. Merely
// replacing getPosition leaves the original player's cached gravity basis.
// Save/restore only target data18..77, not the NameObj header. The global
// MarioHolder scope is also needed by70DE8/70DD0/70E20 (EVD0024/0203).
constexpr std::uintptr_t kPlayerTargetVtable=0x81BAA0;
constexpr std::uintptr_t kPlayerTargetRefresh=0x70C38;
constexpr std::uintptr_t kMarioHolderGet=0x4340;
unsigned sLeaderLogs=0,sLeaderFrames=0;
int sLastLeader=-1;
unsigned short sLeaderNativeState[2]{};
bool sLeaderNativeStateValid[2]{};
class NativeLeaderScope {
    std::uintptr_t mTarget=0;
    std::uintptr_t* mHolderSlot=nullptr;
    std::uintptr_t mHolderPrevious=0;
    unsigned char mSaved[0x60]{};
public:
    NativeLeaderScope(std::uintptr_t cameraMan,void* leader) {
        Vec leaderUp;
        if ((leader!=GetFirstPlayer() && leader!=GetSecondPlayer()) || !PlayerUp(leader,leaderUp)) return;
        auto playerTarget=Pointer(Pointer(Pointer(cameraMan,0x18),0x68),8);
        const auto base=exl::util::modules::GetTargetStart();
        if (!playerTarget || *reinterpret_cast<const std::uintptr_t*>(playerTarget)!=base+kPlayerTargetVtable) return;
        auto* oldActor=reinterpret_cast<void*>(Pointer(playerTarget,0x18));
        if ((oldActor!=GetFirstPlayer() && oldActor!=GetSecondPlayer()) || !IsValidMarioActor(oldActor)) return;
        const auto holder=reinterpret_cast<std::uintptr_t (*)()>(base+kMarioHolderGet)();
        if (!IsGuestPointer(holder)) return;
        auto* slot=reinterpret_cast<std::uintptr_t*>(holder+0x18);
        auto* holderActor=reinterpret_cast<void*>(*slot);
        if ((holderActor!=GetFirstPlayer() && holderActor!=GetSecondPlayer()) || !IsValidMarioActor(holderActor)) return;
        mTarget=playerTarget;mHolderSlot=slot;mHolderPrevious=*slot;
        std::memcpy(mSaved,reinterpret_cast<const void*>(mTarget+0x18),sizeof(mSaved));
        *slot=reinterpret_cast<std::uintptr_t>(leader);
        *reinterpret_cast<void**>(mTarget+0x18)=leader;
        // 70C38 compares previous native state70 before setting changed72.
        // Preserve that small value per player; never retain its heap fields.
        const int index=leader==GetFirstPlayer()?0:1;
        if (sLeaderNativeStateValid[index])
            *reinterpret_cast<unsigned short*>(mTarget+0x70)=sLeaderNativeState[index];
        reinterpret_cast<void (*)(std::uintptr_t)>(base+kPlayerTargetRefresh)(mTarget);
        sLeaderNativeState[index]=*reinterpret_cast<const unsigned short*>(mTarget+0x70);
        sLeaderNativeStateValid[index]=true;
    }
    ~NativeLeaderScope() {
        if (mTarget) std::memcpy(reinterpret_cast<void*>(mTarget+0x18),mSaved,sizeof(mSaved));
        if (mHolderSlot) *mHolderSlot=mHolderPrevious;
    }
    bool Active() const {return mTarget!=0;}
    NativeLeaderScope(const NativeLeaderScope&)=delete;
    NativeLeaderScope& operator=(const NativeLeaderScope&)=delete;
};
void ObserveLeaderView(std::uintptr_t cameraMan,const Vec points[2],bool regrouping,bool bound) {
    const int leader=GetCoopCameraLeaderIndex();
    const auto pose=Pointer(cameraMan,0x20);
    Vec ups[2];
    bool valid=bound && !regrouping && pose && PlayerUp(GetFirstPlayer(),ups[0]) && PlayerUp(GetSecondPlayer(),ups[1]);
    float alignment=1;
    int reason=0;
    if (valid) {
        alignment=std::clamp(Dot(ups[0],ups[1]),-1.0f,1.0f);
        const Vec eye=Read(pose+0x24),watch=Read(pose+0x0C),up=Read(pose+0x18);
        const float fovy=*reinterpret_cast<const float*>(pose+0x30);
        valid=Finite(eye)&&Finite(watch)&&Finite(up)&&std::isfinite(fovy)&&fovy>=10&&fovy<=120&&Length(watch-eye)>1;
        if (valid) {
            // No averaging or orbit search: inspect the native view only.
            if (alignment<0.707107f) reason|=1; // >45 degrees, no sphere inference
            const Vec forward=planet::Unit(watch-eye);
            const Vec right=planet::Unit(Cross(forward,up),planet::Perpendicular(forward));
            const Vec screenUp=planet::Unit(Cross(right,forward));
            const Vec body=points[1-leader]+ups[1-leader]*70;
            const Vec delta=body-eye;
            const float depth=Dot(delta,forward);
            const float vertical=std::tan(fovy*0.5f*(3.14159265f/180.0f))*0.85f;
            if (depth<=1 || std::abs(Dot(delta,right))+70>depth*vertical*(16.0f/9.0f) ||
                std::abs(Dot(delta,screenUp))+70>depth*vertical) reason|=2;
            const auto keeper=MapKeeper();
            if (keeper && !ClearLine(keeper,eye,points[1-leader]+ups[1-leader]*40) &&
                !ClearLine(keeper,eye,points[1-leader]+ups[1-leader]*120)) reason|=4;
        }
    }
    ReportCoopCameraVisibility(valid,reason!=0);
    if (sLeaderLogs<32 && (sLastLeader!=leader || ++sLeaderFrames%120==1)) {
        sLastLeader=leader;++sLeaderLogs;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0169 native leader=p%d target_bound=%d up_dot=%.3f reason=%d visibility_valid=%d regrouping=%d kill=1",
            leader+1,bound?1:0,static_cast<double>(alignment),reason,valid?1:0,regrouping?1:0);
    }
}
// PATCH-0170 / EVD-0204: hybrid selection. Aligned gravity uses the verified
// PATCH-0164 midpoint+zoom inside the PATCH-0169 leader basis scope; diverging
// gravity returns to the pure native leader view and its regroup reasons.
// Thresholds are provisional. No new hook sites or addresses.
constexpr float kHybridEnterDot=0.866025f;  // <=30 degrees
constexpr float kHybridExitDot=0.766044f;   // >40 degrees
constexpr unsigned kHybridEnterTicks=20,kHybridBlendTicks=30,kHybridCooldownTicks=60,kHybridEaseTicks=30;
constexpr float kHybridMaxExtra=8000.0f;
bool sHybridShared=false;
unsigned sHybridAlignedTicks=0,sHybridCooldown=0,sBlendTicks=0,sSharedTicks=0,sHybridLogs=0;
float sHybridAlignment=1;
Vec sHybridUps[2]{};
bool sHybridUpsValid=false;

void ResetHybridMode() {
    sHybridShared=false;sHybridAlignedTicks=0;sHybridCooldown=0;sBlendTicks=0;sSharedTicks=0;
    sHybridAlignment=1;sHybridUpsValid=false;
}
bool UpdateHybridMode(void* p1,void* p2,bool regrouping,const Vec points[2]) {
    Vec ups[2];
    sHybridUpsValid=PlayerUp(p1,ups[0]) && PlayerUp(p2,ups[1]);
    sHybridAlignment=sHybridUpsValid ? std::clamp(Dot(ups[0],ups[1]),-1.0f,1.0f) : -1.0f;
    if (sHybridUpsValid) {sHybridUps[0]=ups[0];sHybridUps[1]=ups[1];}
    if (sHybridCooldown) --sHybridCooldown;
    const bool aligned=sHybridUpsValid && sHybridAlignment>=kHybridEnterDot;
    sHybridAlignedTicks=aligned ? std::min(sHybridAlignedTicks+1,1000u) : 0;
    const bool next=sHybridShared ?
        (sHybridUpsValid && !regrouping && sHybridAlignment>=kHybridExitDot) :
        (!regrouping && !sHybridCooldown && sHybridAlignedTicks>=kHybridEnterTicks);
    if (next!=sHybridShared) {
        const int leader=GetCoopCameraLeaderIndex();
        if (next) {
            // Start from the leader view so entry eases instead of cutting.
            if (!sBlendTicks) {sCenter=points[leader];sHaveCenter=true;sExtraDistance=0;}
            sBlendTicks=0;sSharedTicks=0;
        } else {
            sBlendTicks=kHybridBlendTicks;sHybridCooldown=kHybridCooldownTicks;
        }
        sHybridShared=next;
        if (sHybridLogs++<32)
            Logging.Log("[GalaxyCoop:smg1] PATCH-0170 hybrid mode=%s leader=p%d up_dot=%.3f separation=%.1f regrouping=%d extra=%.1f kill=1",
                next?"shared":"leader",leader+1,static_cast<double>(sHybridAlignment),
                static_cast<double>(Length(points[0]-points[1])),regrouping?1:0,static_cast<double>(sExtraDistance));
    }
    if (sHybridShared && sSharedTicks<1000) ++sSharedTicks;
    return sHybridShared;
}
// Pushes the eye back along the native view direction only; watch, up and
// orientation stay native (PATCH-0164 fit). Reports regroup visibility.
void FitHybridFrame(std::uintptr_t cameraMan,const Vec frame[2],bool shared,bool regrouping,int leader) {
    const auto pose=Pointer(cameraMan,0x20);
    if (!pose) {ReportCoopCameraVisibility(false,false);return;}
    const Vec watch=Read(pose+0x0C), eye=Read(pose+0x24), up=Read(pose+0x18);
    const float fovy=*reinterpret_cast<const float*>(pose+0x30);
    const float nativeDistance=Length(watch-eye);
    if (!Finite(watch)||!Finite(eye)||!Finite(up)||!std::isfinite(fovy)||fovy<10||fovy>120||
        nativeDistance<1||nativeDistance>20000) {ReportCoopCameraVisibility(false,false);return;}
    const Vec forward=(watch-eye)*(1.0f/nativeDistance);
    Vec right=Cross(forward,up);
    const float rightLength=Length(right);
    if (rightLength<0.001f) {ReportCoopCameraVisibility(false,false);return;}
    right=right*(1.0f/rightLength);
    const Vec screenUp=Cross(right,forward);
    const float vertical=std::tan(fovy*0.5f*(3.14159265f/180.0f))*0.80f;
    const float horizontal=vertical*(16.0f/9.0f);
    float required=nativeDistance;
    for (int i=0;i<2;++i) {
        const Vec relative=frame[i]-watch;
        const float depth=Dot(relative,forward);
        required=std::max(required,(std::abs(Dot(relative,right))+90.0f)/horizontal-depth);
        required=std::max(required,(std::abs(Dot(relative,screenUp))+90.0f)/vertical-depth);
    }
    const bool saturated=required-nativeDistance>kHybridMaxExtra;
    const float extra=std::max(0.0f,std::min(required-nativeDistance,kHybridMaxExtra));
    if (extra>sExtraDistance)
        sExtraDistance=sSharedTicks<kHybridEaseTicks ? sExtraDistance+(extra-sExtraDistance)*0.2f : extra;
    else
        sExtraDistance+=(extra-sExtraDistance)*(shared?0.08f:0.15f);
    const Vec newEye=watch-forward*(nativeDistance+sExtraDistance);
    Write(pose+0x24,newEye);
    if (shared) {
        // Offscreen only counts once zoom is saturated; terrain still counts.
        bool blocked=saturated;
        const auto keeper=MapKeeper();
        if (keeper && sHybridUpsValid && !(VisiblePlayers(keeper,newEye,frame,sHybridUps)&(1<<(1-leader)))) blocked=true;
        ReportCoopCameraVisibility(!regrouping,blocked);
    } else {
        // Blend-out after divergence: keep the leader gravity reason live.
        ReportCoopCameraVisibility(!regrouping && sHybridUpsValid,sHybridAlignment<0.707107f);
    }
    const float separation=Length(frame[0]-frame[1]);
    const int bin=static_cast<int>(separation/400.0f);
    if (shared && sFitLogs<24 && bin!=sLastBin) {
        sLastBin=bin; ++sFitLogs;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0170 shared frame separation=%.1f native=%.1f extra=%.1f saturated=%d up_dot=%.3f kill=1",
            static_cast<double>(separation),static_cast<double>(nativeDistance),static_cast<double>(sExtraDistance),
            saturated?1:0,static_cast<double>(sHybridAlignment));
    }
}

// ATT-0140: split policy sample, read from the pose the native calc just wrote
// (before any shared push-back). It asks what a midpoint frame in the same
// basis would need; nothing is written. The gravity gate repeats PATCH-0170's
// thresholds but keeps running while split, when UpdateHybridMode does not.
CoopFramingSample sFramingSample{};
bool sEventCamera=false;
unsigned sOrdinaryCameraSerial=0;
int sNativeViewOwnerIndex=-1; // ATT-0153: value only; never a scene heap pointer.
bool sPolicyAligned=false,sPolicyHaveGate=false,sPolicyBlocked=false;
unsigned sPolicyAlignedTicks=0,sPolicyTicks=0;
// ATT-0142: consecutive blocked visibility samples (one every 6 ticks).
constexpr unsigned kPolicyBlockedSamples=3;
unsigned sPolicySharedRuns=0,sPolicyNativeRuns=0;
void SampleSharedFraming(std::uintptr_t cameraMan,const Vec points[2],void* p1,void* p2) {
    Vec ups[2];
    const auto pose=Pointer(cameraMan,0x20);
    if (!pose || !PlayerUp(p1,ups[0]) || !PlayerUp(p2,ups[1])) return;
    const float dot=std::clamp(Dot(ups[0],ups[1]),-1.0f,1.0f);
    sPolicyAlignedTicks=dot>=kHybridEnterDot ? std::min(sPolicyAlignedTicks+1,1000u) : 0;
    if (!sPolicyHaveGate) sPolicyAligned=dot>=kHybridEnterDot;
    else if (sPolicyAligned) sPolicyAligned=dot>=kHybridExitDot;
    else sPolicyAligned=sPolicyAlignedTicks>=kHybridEnterTicks;
    sPolicyHaveGate=true;
    const Vec nativeWatch=Read(pose+0x0C), eye=Read(pose+0x24), up=Read(pose+0x18);
    const float fovy=*reinterpret_cast<const float*>(pose+0x30);
    const float nativeDistance=Length(nativeWatch-eye);
    if (!Finite(nativeWatch)||!Finite(eye)||!Finite(up)||!std::isfinite(fovy)||fovy<10||fovy>120||
        nativeDistance<1||nativeDistance>20000) return;
    const Vec forward=(nativeWatch-eye)*(1.0f/nativeDistance);
    Vec right=Cross(forward,up);
    const float rightLength=Length(right);
    if (rightLength<0.001f) return;
    right=right*(1.0f/rightLength);
    const Vec screenUp=Cross(right,forward);
    const float vertical=std::tan(fovy*0.5f*(3.14159265f/180.0f))*0.80f;
    const float horizontal=vertical*(16.0f/9.0f);
    const auto extraAround=[&](Vec watch) {
        float required=nativeDistance;
        for (int i=0;i<2;++i) {
            const Vec relative=points[i]-watch;
            const float depth=Dot(relative,forward);
            required=std::max(required,(std::abs(Dot(relative,right))+90.0f)/horizontal-depth);
            required=std::max(required,(std::abs(Dot(relative,screenUp))+90.0f)/vertical-depth);
        }
        return required-nativeDistance;
    };
    const Vec watch=(points[0]+points[1])*0.5f;
    const float extra=extraAround(watch);
    // ATT-0142: the native view keeps its own watch point and eye.
    const float nativeExtra=extraAround(nativeWatch);
    if (sPolicyTicks++%kVisibilitySampleInterval==0) {
        const auto keeper=MapKeeper();
        const Vec sharedEye=watch-forward*(nativeDistance+std::min(extra,kHybridMaxExtra));
        sPolicyBlocked=keeper && VisiblePlayers(keeper,sharedEye,points,ups)!=3;
        const bool nativeBlocked=keeper && VisiblePlayers(keeper,eye,points,ups)!=3;
        sPolicySharedRuns=sPolicyBlocked ? std::min(sPolicySharedRuns+1,1000u) : 0;
        sPolicyNativeRuns=nativeBlocked ? std::min(sPolicyNativeRuns+1,1000u) : 0;
    }
    sFramingSample={true,sPolicyAligned,extra,sPolicyBlocked,nativeExtra,
        sPolicyNativeRuns>=kPolicyBlockedSamples,sPolicySharedRuns>=kPolicyBlockedSamples,Length(points[0]-points[1]),nativeDistance};
}

bool ApplyPlanetCamera(std::uintptr_t pose,const Vec points[2],bool regrouping) {
    if (!kPlanetCameraEnabled) return false;
    const int leader=GetCoopCameraLeaderIndex();
    void* actors[2]={GetFirstPlayer(),GetSecondPlayer()};
    Vec ups[2];
    if (!PlayerUp(actors[leader],ups[leader])) return false;
    if (regrouping) ups[1-leader]=ups[leader];
    else if (!PlayerUp(actors[1-leader],ups[1-leader])) return false;
    const float alignment=std::clamp(Dot(ups[0],ups[1]),-1.0f,1.0f);
    if (!sOrbitActive && (regrouping || alignment>0.985f || Length(points[0]-points[1])<150)) return false;
    const Vec nativeEye=Read(pose+0x24),nativeWatch=Read(pose+0x0C);
    const float fovy=*reinterpret_cast<const float*>(pose+0x30);
    if (!Finite(nativeEye)||!Finite(nativeWatch)||!std::isfinite(fovy)||fovy<10||fovy>120) return false;
    const Vec desiredUp=planet::SharedUp(ups[0],ups[1],leader);
    const float weight=planet::LeaderWeight(alignment);
    Vec desiredWatch=points[leader]*weight+points[1-leader]*(1-weight);
    const bool commonSurface=planet::SurfacePivot(points,ups,desiredUp,desiredWatch);
    if (!sOrbitActive) {
        sOrbitActive=true;sOrbitTicks=0;sCandidateVisibility=3;
        // Native frame originates from P1's target basis. Transport its yaw
        // onto shared gravity, then retain yaw continuously around the planet.
        sOrbitUp=desiredUp;
        Vec tangent=nativeEye-nativeWatch;
        tangent=tangent-ups[0]*Dot(tangent,ups[0]);
        tangent=planet::Unit(tangent,planet::Perpendicular(ups[0]));
        sOrbitTangent=planet::Transport(tangent,ups[0],sOrbitUp);
        sOrbitOut=planet::Unit(sOrbitTangent*0.8f+sOrbitUp*0.6f);
        sOrbitDesired=sOrbitOut;sOrbitWatch=desiredWatch;
    }
    const Vec nextUp=planet::StepDirection(sOrbitUp,desiredUp,0.06f);
    sOrbitTangent=planet::Unit(planet::Transport(sOrbitTangent,sOrbitUp,nextUp));
    sOrbitOut=planet::Unit(planet::Transport(sOrbitOut,sOrbitUp,nextUp));
    sOrbitDesired=planet::Unit(planet::Transport(sOrbitDesired,sOrbitUp,nextUp));
    sOrbitUp=nextUp;
    if (Length(desiredWatch-sOrbitWatch)>2000) sOrbitWatch=desiredWatch;
    else sOrbitWatch=sOrbitWatch+(desiredWatch-sOrbitWatch)*0.18f;
    const float minimum=std::clamp(Length(nativeEye-nativeWatch),700.0f,1800.0f);
    const auto keeper=MapKeeper();
    if (sOrbitTicks++%kVisibilitySampleInterval==0 && keeper) {
        Vec best=sOrbitDesired;
        int bestMask=0;float bestScore=-1.0e9f;
        for(int candidate=0;candidate<6;++candidate) {
            Vec outward=sOrbitDesired;
            if (candidate>0) {
                const float pitch=candidate==2 ? 0.95f : candidate==3 ? 1.3f : 0.6f;
                const float yaw=candidate==4 ? -0.65f : candidate==5 ? 0.65f : 0;
                const Vec tangent=planet::Rotate(sOrbitTangent,sOrbitUp,yaw);
                outward=planet::Unit(tangent*std::cos(pitch)+sOrbitUp*std::sin(pitch));
            }
            const float d=planet::FitDistance(points,sOrbitWatch,outward,sOrbitUp,fovy,minimum);
            const int mask=VisiblePlayers(keeper,sOrbitWatch+outward*d,points,ups);
            const float score=((mask&(1<<leader))?120.0f:0)+((mask&(1<<(1-leader)))?100.0f:0)+
                Dot(outward,sOrbitOut)*12.0f-(candidate==0?0.0f:1.0f);
            if (score>bestScore) {bestScore=score;best=outward;bestMask=mask;}
        }
        sOrbitDesired=best;sCandidateVisibility=bestMask;
    }
    sOrbitOut=planet::StepDirection(sOrbitOut,sOrbitDesired,0.045f);
    const float distance=planet::FitDistance(points,sOrbitWatch,sOrbitOut,sOrbitUp,fovy,minimum);
    const Vec eye=sOrbitWatch+sOrbitOut*distance;
    const Vec right=planet::Unit(Cross(sOrbitOut*-1,sOrbitUp),planet::Perpendicular(sOrbitUp));
    const Vec screenUp=planet::Unit(Cross(right,sOrbitOut*-1));
    if (!Finite(eye)||!Finite(screenUp)) return false;
    Write(pose+0x0C,sOrbitWatch);Write(pose+0x24,eye);Write(pose+0x18,screenUp);
    // Regroup only when orbit search cannot frame both without obstruction.
    // Native recovery is queued in the global player tick, never in calc().
    // Visible heads alone are insufficient when the playable surfaces face
    // away from each other. Beyond ~105 degrees on a fitted common planet,
    // regroup even if a high camera can still glimpse both silhouettes.
    const bool surfaceSplit=commonSurface && alignment<-0.258819f;
    const bool blocked=sCandidateVisibility!=3 || surfaceSplit;
    ReportCoopCameraVisibility(keeper && !regrouping,blocked);
    if (sOrbitLogs<24 && sOrbitTicks%60==1) {
        ++sOrbitLogs;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0168 planet orbit leader=p%d up_dot=%.3f visible_mask=%d map_ready=%d surface_split=%d regrouping=%d kill=1",
                    leader+1,static_cast<double>(alignment),sCandidateVisibility,keeper?1:0,surfaceSplit?1:0,regrouping?1:0);
    }
    return true;
}

HOOK_DEFINE_REPLACE(SharedCameraPosition) {
    static const float* Callback(std::uintptr_t cameraTarget) {
        if (sInsideSharedCalc) return &sCenter.x;
        // Preserve the verified surviving-player view and select the pipe
        // participant while the partner waits. No global midpoint override.
        if (HasActivePlayerPair() && !IsP2Parked()) {
            if (auto* leader=GetCoopPipeLeader(); IsValidMarioActor(leader)) return ActorCamera(leader);
            if (IsP1Dead() && !IsP2Dead()) return ActorCamera(GetSecondPlayer());
            if (IsP2Dead() && !IsP1Dead()) return ActorCamera(GetFirstPlayer());
        }
        if (IsGuestPointer(cameraTarget)) {
            const auto actor=*reinterpret_cast<const std::uintptr_t*>(cameraTarget+0x18);
            if (IsGuestPointer(actor)) return reinterpret_cast<const float*>(actor+kCameraPosition);
        }
        static const float fallback[3]={};
        return fallback;
    }
};

// DIAG-0179 kill switch: logs which PATCH-0169/0170 entry gate holds after
// the Honeyhive coin cutscene left the camera on P1 (EVD-0222). Read-only.
constexpr bool kDiag0179CameraGateProbeEnabled=true;
unsigned sGateMask=~0u,sGateLogs=0;
std::uintptr_t HostVtableOffset(std::uintptr_t sensor,std::uintptr_t base) {
    const auto host=Pointer(sensor,0x30);
    const auto vptr=Pointer(host,0);
    return vptr>=base ? vptr-base : 0;
}
// PATCH-0178 kill switch (EVD-0232): a Karikari (vtable 0x862C98) clinging to
// a player sets the +0xF20 binding and stayed set after it was shaken off. It
// does not own the camera, so only other binding hosts suspend co-op framing.
constexpr bool kPatch0178IgnoreKarikariBindingEnabled=true;
constexpr std::uintptr_t kKarikariVtable=0x862C98;
bool HasCameraOwningBinding(void* actor,std::uintptr_t base) {
    if (!HasActiveBinding(reinterpret_cast<std::uintptr_t>(actor))) return false;
    const auto sensor=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(actor)+0xF20);
    if (kPatch0178IgnoreKarikariBindingEnabled && IsGuestPointer(sensor) && HostVtableOffset(sensor,base)==kKarikariVtable) return false;
    return true;
}
void ProbeCameraGates(std::uintptr_t cameraMan,void* p1,void* p2,bool ordinary,bool leaderRegroup) {
    if (!kDiag0179CameraGateProbeEnabled || sGateLogs>=48) return;
    const auto base=exl::util::modules::GetTargetStart();
    const bool valid=IsValidMarioActor(p1) && IsValidMarioActor(p2);
    unsigned mask=0;
    std::uintptr_t f20[2]={0,0};
    if (!ordinary) mask|=1;
    if (IsP2Parked()) mask|=2;
    if (GetCoopPipeLeader()) mask|=4;
    if (!valid) mask|=8;
    else {
        if (!Participating(p1,GetP1DeathPhase())) mask|=0x10;
        if (!Participating(p2,GetP2DeathPhase())) mask|=0x20;
        f20[0]=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(p1)+0xF20);
        f20[1]=*reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(p2)+0xF20);
        if (f20[0]) mask|=0x40;
        if (f20[1]) mask|=0x80;
    }
    if (leaderRegroup) mask|=0x100;
    if (mask==sGateMask) return;
    sGateMask=mask;++sGateLogs;
    const auto vptr=Pointer(cameraMan,0);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0179 camera gates=0x%x man_vt=0x%lx f20=(%p,%p) host_vt=(0x%lx,0x%lx) phase=(%d,%d) sample=%u/48 kill=1",
        mask,static_cast<unsigned long>(vptr>=base?vptr-base:0),
        reinterpret_cast<void*>(f20[0]),reinterpret_cast<void*>(f20[1]),
        static_cast<unsigned long>(IsGuestPointer(f20[0])?HostVtableOffset(f20[0],base):0),
        static_cast<unsigned long>(IsGuestPointer(f20[1])?HostVtableOffset(f20[1],base):0),
        static_cast<int>(GetP1DeathPhase()),static_cast<int>(GetP2DeathPhase()),sGateLogs);
}

HOOK_DEFINE_TRAMPOLINE(SharedCameraCalc) {
    static void Callback(std::uintptr_t cameraMan) {
        void* p1=GetFirstPlayer(); void* p2=GetSecondPlayer();
        const auto target=exl::util::modules::GetTargetStart();
        TickDiag0182();
        TickDiag0183();
        sFramingSample.valid=false;
        sNativeViewOwnerIndex=-1;
        sEventCamera=IsGuestPointer(cameraMan) && *reinterpret_cast<const std::uintptr_t*>(cameraMan)!=target+kCameraVtable;
        if (IsGuestPointer(cameraMan) && !sEventCamera) ++sOrdinaryCameraSerial;
        ProbeCameraGates(cameraMan,p1,p2,
            IsGuestPointer(cameraMan) && *reinterpret_cast<const std::uintptr_t*>(cameraMan)==target+kCameraVtable,
            IsCoopRegrouping());
        ReportCoopCameraVisibility(false,false);
        const bool ordinary=IsGuestPointer(cameraMan) && *reinterpret_cast<const std::uintptr_t*>(cameraMan)==target+kCameraVtable;
        const bool leaderRegroup=IsCoopRegrouping();
        if (kNativeLeaderCameraEnabled && ordinary && !IsP2Parked() && !GetCoopPipeLeader() &&
            IsValidMarioActor(p1) && IsValidMarioActor(p2) &&
            ((Participating(p1,GetP1DeathPhase()) && Participating(p2,GetP2DeathPhase())) || leaderRegroup)) {
            const Vec leaderPoints[2]={Read(reinterpret_cast<std::uintptr_t>(p1)+kCameraPosition),Read(reinterpret_cast<std::uintptr_t>(p2)+kCameraPosition)};
            // Native bindings own their camera until released (pipe/Launch Star).
            const bool unbound=leaderRegroup || (!HasCameraOwningBinding(p1,target) && !HasCameraOwningBinding(p2,target));
            if (unbound && Finite(leaderPoints[0]) && Finite(leaderPoints[1])) {
                bool bound=false;
                // ATT-0137: while split the left half is P1's own native view and
                // pass 1 re-views it at P2: no shared zoom, no visibility regroup.
                // ATT-0140: a shared frame in progress eases back onto P1 (blend-out)
                // instead of cutting, and P1 stays leader so the merge resumes
                // from the same view.
                const bool split=IsCoopSplitActive() || IsCoopSplitPreparing();
                if (split) {
                    HoldCoopCameraLeader(0);
                    if (sHybridShared) {sHybridShared=false;sBlendTicks=kHybridBlendTicks;}
                    sHybridCooldown=0;
                    sHybridAlignedTicks=sPolicyAligned ? kHybridEnterTicks : 0;
                }
                const int leaderIndex=split ? 0 : GetCoopCameraLeaderIndex();
                const bool shared=!split && kHybridSharedCameraEnabled && UpdateHybridMode(p1,p2,leaderRegroup,leaderPoints);
                if (shared || sBlendTicks) {
                    Vec frame[2]={leaderPoints[0],leaderPoints[1]};
                    if (!shared) {frame[1-leaderIndex]=frame[leaderIndex];--sBlendTicks;}
                    const Vec desired=(frame[0]+frame[1])*0.5f;
                    if (!sHaveCenter || Length(desired-sCenter)>2000.0f) sCenter=desired;
                    else sCenter=sCenter+(desired-sCenter)*(shared?0.15f:0.25f);
                    sHaveCenter=true;
                    {
                        NativeLeaderScope scope(cameraMan,leaderIndex==0?p1:p2);
                        bound=scope.Active();
                        // Target refresh ran above with actor position; only
                        // the native calc sees the shared center.
                        const bool previous=sInsideSharedCalc;
                        sInsideSharedCalc=bound;
                        Orig(cameraMan);
                        sInsideSharedCalc=previous;
                    }
                    NotifyCameraSeen(cameraMan);
                    if (bound) sNativeViewOwnerIndex=leaderIndex;
                    if (bound) SampleSharedFraming(cameraMan,leaderPoints,p1,p2);
                    if (bound) FitHybridFrame(cameraMan,frame,shared,leaderRegroup,leaderIndex);
                    else ReportCoopCameraVisibility(false,false);
                    if (split) ReportCoopCameraVisibility(false,false);
                    return;
                }
                sHaveCenter=false;sExtraDistance=0;
                {
                    NativeLeaderScope scope(cameraMan,leaderIndex==0?p1:p2);
                    bound=scope.Active();
                    Orig(cameraMan);
                }
                NotifyCameraSeen(cameraMan);
                if (bound) sNativeViewOwnerIndex=leaderIndex;
                if (bound) SampleSharedFraming(cameraMan,leaderPoints,p1,p2);
                if (split) ReportCoopCameraVisibility(false,false);
                else ObserveLeaderView(cameraMan,leaderPoints,leaderRegroup,bound);
                return;
            }
        }
        if (sHybridShared || sBlendTicks) {ResetHybridMode();sHaveCenter=false;sExtraDistance=0;}
        const bool regrouping=kPlanetCameraEnabled && IsCoopRegrouping();
        const bool share=kSharedCameraEnabled && !IsP2Parked() &&
            ((Participating(p1,GetP1DeathPhase()) && Participating(p2,GetP2DeathPhase())) ||
             (regrouping && IsValidMarioActor(p1) && IsValidMarioActor(p2))) &&
            !GetCoopPipeLeader() && IsGuestPointer(cameraMan) &&
            *reinterpret_cast<const std::uintptr_t*>(cameraMan)==target+kCameraVtable;
        Vec points[2]{};
        bool fit=share;
        if (fit) {
            points[0]=Read(reinterpret_cast<std::uintptr_t>(p1)+kCameraPosition);
            points[1]=Read(reinterpret_cast<std::uintptr_t>(p2)+kCameraPosition);
            if (regrouping) points[1-GetCoopCameraLeaderIndex()]=points[GetCoopCameraLeaderIndex()];
            fit=Finite(points[0])&&Finite(points[1]);
        }
        if (fit) {
            Vec desired=(points[0]+points[1])*0.5f;
            Vec ups[2];
            if (kPlanetCameraEnabled && PlayerUp(p1,ups[0]) && PlayerUp(p2,ups[1]) && !regrouping) {
                const Vec sharedUp=planet::SharedUp(ups[0],ups[1],GetCoopCameraLeaderIndex());
                planet::SurfacePivot(points,ups,sharedUp,desired);
            }
            // Discontinuities are stage/pipe teleports, not a camera pan.
            if (!sHaveCenter || Length(desired-sCenter)>2000.0f) sCenter=desired;
            else sCenter=sCenter+(desired-sCenter)*0.15f;
            sHaveCenter=true;
        } else {
            sHaveCenter=false; sExtraDistance=0;sOrbitActive=false;
        }
        {
            struct Scope {
                bool previous;
                explicit Scope(bool active):previous(sInsideSharedCalc){sInsideSharedCalc=active;}
                ~Scope(){sInsideSharedCalc=previous;}
            } scope(fit);
            Orig(cameraMan);
        }
        NotifyCameraSeen(cameraMan); // observation alone never releases arrival
        if (!fit) return;
        const auto pose=*reinterpret_cast<const std::uintptr_t*>(cameraMan+0x20);
        if (!IsGuestPointer(pose)) return;
        if (ApplyPlanetCamera(pose,points,regrouping)) return;
        const Vec watch=Read(pose+0x0C), eye=Read(pose+0x24), up=Read(pose+0x18);
        const float fovy=*reinterpret_cast<const float*>(pose+0x30);
        if (!Finite(watch)||!Finite(eye)||!Finite(up)||!std::isfinite(fovy)||fovy<10||fovy>120) return;
        const float nativeDistance=Length(watch-eye);
        if (nativeDistance<1 || nativeDistance>20000) return;
        const Vec forward=(watch-eye)*(1.0f/nativeDistance);
        Vec right=Cross(forward,up);
        const float rightLength=Length(right);
        if (rightLength<0.001f) return;
        right=right*(1.0f/rightLength);
        const Vec screenUp=Cross(right,forward);
        const float vertical=std::tan(fovy*0.5f*(3.14159265f/180.0f))*0.80f;
        const float horizontal=vertical*(16.0f/9.0f);
        float required=nativeDistance;
        // Fit bodies, not just their center points. Account for forward depth
        // as well as horizontal/vertical separation on curved planetoids.
        for (Vec point:points) {
            const Vec relative=point-watch;
            const float depth=Dot(relative,forward);
            required=std::max(required,(std::abs(Dot(relative,right))+90.0f)/horizontal-depth);
            required=std::max(required,(std::abs(Dot(relative,screenUp))+90.0f)/vertical-depth);
        }
        const float extra=std::max(0.0f,std::min(required-nativeDistance,8000.0f));
        // Expand immediately to retain both players; ease inward at reunion.
        if (extra>sExtraDistance) sExtraDistance=extra;
        else sExtraDistance+=(extra-sExtraDistance)*0.08f;
        Write(pose+0x24,watch-forward*(nativeDistance+sExtraDistance));
        const float separation=Length(points[0]-points[1]);
        const int bin=static_cast<int>(separation/400.0f);
        if (sFitLogs<24 && bin!=sLastBin) {
            sLastBin=bin; ++sFitLogs;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0164 shared frame separation=%.1f native=%.1f extra=%.1f fovy=%.1f arrival_released=1 kill=1",
                static_cast<double>(separation),static_cast<double>(nativeDistance),static_cast<double>(sExtraDistance),static_cast<double>(fovy));
        }
    }
};
}
void ResetSharedCamera() {
    sHaveCenter=false; sInsideSharedCalc=false; sExtraDistance=0; sFitLogs=0; sLastBin=-1;
    sOrbitActive=false;sOrbitTicks=0;sOrbitLogs=0;sCandidateVisibility=3;
    sLeaderLogs=0;sLeaderFrames=0;sLastLeader=-1;
    for (int i=0;i<2;++i) {sLeaderNativeState[i]=0;sLeaderNativeStateValid[i]=false;}
    ResetHybridMode();sHybridLogs=0;sGateMask=~0u;
    sFramingSample={};sEventCamera=false;sPolicyAligned=false;sPolicyHaveGate=false;sPolicyBlocked=false;
    sNativeViewOwnerIndex=-1;
    sPolicyAlignedTicks=0;sPolicyTicks=0;
    ReportCoopCameraVisibility(false,false);
}
CoopFramingSample GetCoopFramingSample() {return sFramingSample;}
bool IsCoopEventCamera() {return sEventCamera;}
unsigned GetCoopOrdinaryCameraSerial() {return sOrdinaryCameraSerial;}
int GetCoopNativeViewOwnerIndex() {return sNativeViewOwnerIndex;}
bool IsCoopSharedFraming() {return kNativeLeaderCameraEnabled && kHybridSharedCameraEnabled && sHybridShared;}
bool GetCoopPlayerUp(void* actor,float up[3]) {
    Vec value;
    if (!PlayerUp(actor,value)) return false;
    up[0]=value.x;up[1]=value.y;up[2]=value.z;
    return true;
}
void InstallCamera(const galaxy::target::Identity& identity) {
    if (identity.game!=galaxy::target::Game::Smg1) return;
    if (kSharedCameraEnabled||kNativeLeaderCameraEnabled||kBubbleRespawnEnabled||kPipeCompanionEnabled)
        SharedCameraPosition::InstallAtOffset(kGetPosition);
    SharedCameraCalc::InstallAtOffset(kCameraCalc);
    if (kNativeLeaderCameraEnabled)
        Logging.Log("[GalaxyCoop:smg1] PATCH-0169 native leader camera installed initial=12 handoff=60 regroup_grace=30 angle=45 distance=1800 kill=1");
    if (kNativeLeaderCameraEnabled && kHybridSharedCameraEnabled)
        Logging.Log("[GalaxyCoop:smg1] PATCH-0170 hybrid shared camera installed enter=30deg/20 exit=40deg blend=30 cooldown=60 max_extra=8000 shared_distance=3000 kill=1");
    if (kPlanetCameraEnabled)
        Logging.Log("[GalaxyCoop:smg1] PATCH-0168 planet orbit + native map visibility installed grace=90 kill=1");
    Logging.Log("[GalaxyCoop:smg1] PATCH-0178 camera ignores Karikari binding host_vt=0x%lx kill=%d",
        static_cast<unsigned long>(kKarikariVtable),kPatch0178IgnoreKarikariBindingEnabled?1:0);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0180 binding gates require +0xF38 rush-active kill=%d",
        kPatch0180ActiveBindingOnlyEnabled?1:0);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0179 camera gate probe armed kill=%d",kDiag0179CameraGateProbeEnabled?1:0);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0164 shared_camera=%d ordinary_calc_scope=1 arrival_gate=1 spawn_target_reset=0 kill=1",kSharedCameraEnabled?1:0);
}
}
