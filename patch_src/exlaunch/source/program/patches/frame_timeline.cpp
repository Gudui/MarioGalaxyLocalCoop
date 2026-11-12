#include "frame_timeline.hpp"
#if defined(GALAXY_FRAME_TIMELINE_DIAG) && GALAXY_FRAME_TIMELINE_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Frame timeline worker join requires the production GPU owner anchor
#endif
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
#include <atomic>
namespace mod::patches {
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD-0466: frame loop 97790 calls each phase once, in this order, from a
// single bl site (977E0/977F0/97800/97818/97860/9786C). All entries follow a
// ret/b, open with stp/str/mov/ldr(b/h) only (relocatable first16B), and take
// x0 only; four registers are forwarded unchanged anyway. Observe durations
// only: every call, argument and return value is forwarded.
constexpr std::uintptr_t kLoop=0x97790,kBegin=0x98F20,kDraw=0x98154,kEnd=0x9842C;
constexpr std::uintptr_t kCalc=0x97A2C,kFrameEnd=0x97BF0,kPace=0x978E4;
// EVD-0320/0457: backend VT889C68 +DA8 -> native worker timer; owner=timer-4F2D58
// has VT887C10. Completed frames publish +68 count, +70 start, +78 end and
// +80..98 decoder/idle/fence/present ticks. Read-only, double-read, no mutex.
// 4574/C86C: live phase id +0 (1 decode,2 idle,3 fence,4 present), phase start
// +8, running totals +28/+30/+38/+40 zeroed after each publish.
constexpr std::uintptr_t kBackendVT=0x889C68,kOwnerVT=0x887C10;
// EVD-0457/0472: owner constructor 214E00 initializes two timers with 21484C:
// producer owner+4F2CB8 (GPU_Decoding thread 4E0490 -> 5048, FIFO bytes to
// packets) and consumer owner+4F2D58 (4574 -> 17AD0, packets to GPU). Same
// layout, both published by C86C. Context owner+3B0 (+1C710 == backend, EVD-0458)
// holds the packet queue: consumer cursor u32 +1B3C, producer cursor u32 +1B40.
constexpr std::uintptr_t kConsumerTimer=0x4F2D58,kProducerTimer=0x4F2CB8,kContext=0x3B0;
constexpr std::uintptr_t kContextBackend=0x1C710,kQueueRead=0x1B3C,kQueueWrite=0x1B40;
enum Stage:unsigned{Consumer,Producer,StageCount};
constexpr const char* kStageName[StageCount]={"worker","producer"};
// EVD-0471: .data 887CC8 slot 526BC loads [[x0+8]+0x48] and branches to 526C8,
// which posts PE events to queue [obj+0x88]: w1==1 sends 0x10000 (draw done,
// relayed by GraphicsEventHanlder 4C1990 to 4BFB30, which releases the 97FD0
// wait), w1==0 the PE token. C86C publishes a stage frame (x0=timer). Openings:
// 526C8 stp/str/mov/mov, C86C four stp (relocatable first16B). Observe only;
// x0..x3 forwarded.
constexpr std::uintptr_t kPePost=0x526C8,kPublish=0xC86C;
enum Phase:unsigned{Begin,Draw,End,Calc,FrameEnd,Pace,PhaseCount};
// Main segments in frame order: loop->begin, begin, begin->draw, draw,
// draw->calc (incl. render end), calc, calc->frame end, frame end wait,
// frame end->pace, pace, pace->next loop.
enum Segment:unsigned{Pre,SBegin,Prep,SDraw,Mid,SCalc,CalcTail,SFrameEnd,PrePace,SPace,Tail,SegmentCount};
enum State:unsigned{Merged,Split,Trans,NonCoop,NoScene,StateCount};
constexpr unsigned kAllPhases=(1u<<PhaseCount)-1;
constexpr unsigned kWindow[StateCount]={300,300,60,300,300};
constexpr const char* kStateName[StateCount]={"merged","split","transition","noncoop","noscene"};
constexpr unsigned kMaxLines=1200;
// Idle states (no co-op, no scene) get their own budget so hours at a title or
// single-player screen cannot use up the split/merged windows (EVD-0491).
constexpr unsigned kMaxIdleStateLines=360;
// Stage join groups over main segments; boundary snapshot index per phase
// entry/exit (render end 9842C lies inside Mid and takes none).
enum Group:unsigned{GStart,GDraw,GMid,GCalc,GCalcTail,GFrameEnd,GPost,GroupCount};
constexpr unsigned kGroupOf[SegmentCount]={GStart,GStart,GStart,GDraw,GMid,GCalc,GCalcTail,GFrameEnd,GPost,GPost,GPost};
constexpr unsigned kNoSnap=0xFF;
constexpr unsigned kSnapIn[PhaseCount]={1,3,kNoSnap,5,7,9},kSnapOut[PhaseCount]={2,4,kNoSnap,6,8,10};
struct TimerSnap{bool ok=false;unsigned phase=0;std::uint64_t count=0,live[3]{},published[3]{};};
struct Snapshot{TimerSnap t[StageCount]{};bool queueOk=false;std::uint32_t read=0,write=0;};
struct StageWindow{
    std::uint32_t n=0,missed=0,rejected=0,joinN=0,joinBad=0;
    std::uint64_t sum[5]{},maximum=0,join[GroupCount][3]{};
};
// Boundary samples (12 per frame): consumer idle on an empty or a non-empty
// queue, producer idle, both idle, producer decoding while the consumer idles;
// queue backlog bytes at frame end entry.
struct QueueWindow{
    std::uint32_t samples=0,consumerIdleEmpty=0,consumerIdleQueued=0,producerIdle=0,bothIdle=0,producerBusy=0;
    std::uint32_t fendN=0,wrapped=0;std::uint64_t fendBacklog=0;
};
// Draw-done post inside the frame end wait: fend entry -> post (dPost), post ->
// fend exit (relay + wake), each stage's idle/decode/fence before and after.
struct PostWindow{
    std::uint32_t n=0,none=0,multi=0,outside=0,torn=0,joinN[StageCount]{},joinBad[StageCount]{},phase[StageCount][6]{};
    std::uint32_t thrMain=0,thrWorker=0,thrOther=0,thrUnknown=0,lrCount[4]{},lrOther=0,done0=0,token0=0;
    std::uint64_t post[300]{},wake[300]{},before[StageCount][3]{},after[StageCount][3]{},lr[4]{};
};
struct Window{
    std::uint32_t n=0,skipped=0,idleAtFrameEnd=0;
    std::uint64_t sum[SegmentCount]{},maximum[SegmentCount]{},intervals[300]{};
    StageWindow stage[StageCount]{};
    QueueWindow queue{};
    PostWindow post{};
};
Window sWindow[StateCount]{};
std::uint64_t sIn[PhaseCount]{},sOut[PhaseCount]{},sFrameStart=0,sFrequency=0,sStageCount[StageCount]{};
std::uintptr_t sThread=0,sTimer[StageCount]{},sContext=0,sWorkerThread=0,sProducerThread=0;
// Seqlock record written by the posting thread, read by main at fend exit.
struct PostRecord{std::uint64_t tick=0;std::uintptr_t thread=0,lr=0;Snapshot snap{};};
std::atomic<std::uint32_t> sPostSeq{0},sDoneCount{0},sTokenCount{0};
PostRecord sPost{},sPostLocal{};
std::uint32_t sPostSeqIn=0,sPostSeqOut=0;
bool sPostTorn=false;
Snapshot sSnap[SegmentCount+1]{};
unsigned sMask=0,sRepeat=0,sScenes=0,sForeign=0,sLines=0,sPrevState=NoScene,sAnchorMisses=0;
unsigned sStateLines[StateCount]{};
bool sActive=false,sAvailable=false,sValid=false,sLogged=false;
float sReveal=0;
std::atomic<std::uint32_t> sFrameId{0};
std::atomic<bool> sSplitFrame{false};
std::uintptr_t Main(){return exl::util::modules::GetTargetStart();}
std::uintptr_t Thread(){std::uintptr_t v;asm volatile("mrs %0,tpidrro_el0":"=r"(v));return v;}
std::uint64_t Clock(){std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
template<class T>T Read(std::uintptr_t p,std::uintptr_t off){return *reinterpret_cast<const volatile T*>(p+off);}
double Ms(std::uint64_t ticks){return static_cast<double>(ticks)*1000.0/static_cast<double>(sFrequency);}
std::uint64_t Ticks(double ms){return static_cast<std::uint64_t>(ms*static_cast<double>(sFrequency)/1000.0);}
// Validates backend -> consumer timer -> owner VT, then derives the producer
// timer and the context (accepted only when its +1C710 is the same backend).
bool Anchor(std::uintptr_t (&timers)[StageCount],std::uintptr_t& context){
    const auto backend=static_cast<std::uintptr_t>(gGalaxyPushFilter.backend);
    if(!IsGuestPointer(backend)||Read<std::uintptr_t>(backend,0)!=Main()+kBackendVT)return false;
    const auto timer=Read<std::uintptr_t>(backend,0xDA8);
    if(!IsGuestPointer(timer)||timer<kConsumerTimer||!IsGuestPointer(timer+0xA0))return false;
    const auto owner=timer-kConsumerTimer;
    if(!IsGuestPointer(owner)||Read<std::uintptr_t>(owner,0)!=Main()+kOwnerVT)return false;
    timers[Consumer]=timer;timers[Producer]=owner+kProducerTimer;
    const auto ctx=Read<std::uintptr_t>(owner,kContext);
    context=IsGuestPointer(ctx)&&IsGuestPointer(ctx+kContextBackend)&&
        Read<std::uintptr_t>(ctx,kContextBackend)==backend?ctx:0;
    return true;
}
void SampleStage(StageWindow& w,unsigned t){
    const auto timer=sTimer[t];
    if(!timer){if(t==Consumer)++sAnchorMisses;return;}
    std::uint64_t a[7],b[7];
    for(unsigned i=0;i<7;++i)a[i]=Read<std::uint64_t>(timer,0x68+i*8);
    asm volatile("":::"memory");
    for(unsigned i=0;i<7;++i)b[i]=Read<std::uint64_t>(timer,0x68+i*8);
    if(!std::equal(a,a+7,b)){++w.rejected;return;}
    auto& count=sStageCount[t];
    if(a[0]==count)return;
    const bool first=!count;
    if(!first&&a[0]>count+1)w.missed+=static_cast<std::uint32_t>(a[0]-count-1);
    count=a[0];
    if(first)return;
    const auto duration=a[2]-a[1];
    const auto phases=a[3]+a[4]+a[5]+a[6];
    if(a[2]<=a[1]||phases>duration||duration>sFrequency){++w.rejected;return;}
    ++w.n;w.sum[0]+=duration;w.maximum=std::max(w.maximum,duration);
    for(unsigned i=0;i<4;++i)w.sum[1+i]+=a[3+i];
}
TimerSnap SnapTimer(std::uintptr_t timer,std::uint64_t now){
    TimerSnap s{};
    if(!timer)return s;
    s.count=Read<std::uint64_t>(timer,0x68);
    const int phase=Read<int>(timer,0);
    const auto start=Read<std::uint64_t>(timer,8);
    std::uint64_t acc[3],pub[3];
    for(unsigned i=0;i<3;++i){acc[i]=Read<std::uint64_t>(timer,0x28+i*8);pub[i]=Read<std::uint64_t>(timer,0x80+i*8);}
    if(Read<std::uint64_t>(timer,0x68)!=s.count||phase<0||phase>4)return s;
    // C86C copies the closed totals before zeroing them outside its mutex.
    if(!phase&&std::equal(acc,acc+3,pub))std::fill(acc,acc+3,0);
    s.phase=static_cast<unsigned>(phase);
    for(unsigned i=0;i<3;++i){
        s.live[i]=acc[i]+(phase==static_cast<int>(i+1)&&now>=start?now-start:0);
        s.published[i]=pub[i];
    }
    s.ok=true;return s;
}
// Also called on the posting thread; main publishes only validated anchors.
Snapshot Snap(std::uint64_t now){
    Snapshot s{};
    for(unsigned t=0;t<StageCount;++t)s.t[t]=SnapTimer(__atomic_load_n(&sTimer[t],__ATOMIC_RELAXED),now);
    const auto context=__atomic_load_n(&sContext,__ATOMIC_RELAXED);
    if(context){s.read=Read<std::uint32_t>(context,kQueueRead);s.write=Read<std::uint32_t>(context,kQueueWrite);s.queueOk=true;}
    return s;
}
// Stage decode/idle/fence ticks between two main boundaries; false if a
// publish raced, two publishes intervened or a running total went backwards.
bool StageDelta(const TimerSnap& a,const TimerSnap& b,std::uint64_t (&d)[3]){
    if(!a.ok||!b.ok)return false;
    for(unsigned i=0;i<3;++i){
        if(b.count==a.count){if(b.live[i]<a.live[i])return false;d[i]=b.live[i]-a.live[i];}
        else if(b.count==a.count+1){if(b.published[i]<a.live[i])return false;d[i]=b.published[i]-a.live[i]+b.live[i];}
        else return false;
    }
    return true;
}
void LogStage(unsigned state,unsigned t,const StageWindow& s){
    const double n=s.n?s.n:1;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0208 %s state=%s n=%u missed=%u rej=%u anchor_miss=%u dur=%.2f/%.2f "
        "decode=%.2f idle=%.2f fence=%.2f present=%.2f kill=1",
        kStageName[t],kStateName[state],s.n,s.missed,s.rejected,sAnchorMisses,Ms(s.sum[0])/n,Ms(s.maximum),
        Ms(s.sum[1])/n,Ms(s.sum[2])/n,Ms(s.sum[3])/n,Ms(s.sum[4])/n);
}
void Flush(unsigned state){
    auto& w=sWindow[state];
    std::uint64_t sorted[300];
    std::copy(w.intervals,w.intervals+w.n,sorted);
    std::sort(sorted,sorted+w.n);
    // Buckets: early <15.5ms, one vsync 15.5-17.8, late 17.8-22, 22-30, two vsyncs 30-36, >=36.
    const std::uint64_t edge[5]={Ticks(15.5),Ticks(17.8),Ticks(22.0),Ticks(30.0),Ticks(36.0)};
    unsigned hist[6]{};
    for(unsigned i=0;i<w.n;++i){unsigned b=0;while(b<5&&sorted[i]>=edge[b])++b;++hist[b];}
    double total=0;
    for(unsigned s=0;s<SegmentCount;++s)total+=Ms(w.sum[s]);
    const double n=w.n;
    auto mean=[&](unsigned s){return Ms(w.sum[s])/n;};
    auto peak=[&](unsigned s){return Ms(w.maximum[s]);};
    Logging.Log("[GalaxyCoop:smg1] DIAG-0208 frame timeline v2 state=%s n=%u skip=%u fps=%.2f int=(%.2f p50=%.2f p95=%.2f max=%.2f) "
        "hist=(%u,%u,%u,%u,%u,%u) pre=%.2f begin=%.2f prep=%.2f draw=%.2f/%.2f mid=%.2f calc=%.2f/%.2f ctail=%.2f "
        "fend=%.2f/%.2f prepace=%.2f pace=%.2f tail=%.2f thr=%u kill=1",
        kStateName[state],w.n,w.skipped,n*1000.0/total,total/n,Ms(sorted[w.n/2]),Ms(sorted[(w.n*95)/100]),Ms(sorted[w.n-1]),
        hist[0],hist[1],hist[2],hist[3],hist[4],hist[5],mean(Pre),mean(SBegin),mean(Prep),mean(SDraw),peak(SDraw),mean(Mid),
        mean(SCalc),peak(SCalc),mean(CalcTail),mean(SFrameEnd),peak(SFrameEnd),mean(PrePace),mean(SPace),mean(Tail),sForeign);
    LogStage(state,Consumer,w.stage[Consumer]);
    LogStage(state,Producer,w.stage[Producer]);
    const auto& c=w.stage[Consumer];
    const double cn=c.joinN?c.joinN:1;
    auto g=[&](unsigned grp,unsigned k){return Ms(c.join[grp][k])/cn;};
    Logging.Log("[GalaxyCoop:smg1] DIAG-0208 worker-in state=%s n=%u bad=%u idle=(%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f) "
        "decode=(%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f) fence=(%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f) idle_at_fend=%u kill=1",
        kStateName[state],c.joinN,c.joinBad,g(0,1),g(1,1),g(2,1),g(3,1),g(4,1),g(5,1),g(6,1),
        g(0,0),g(1,0),g(2,0),g(3,0),g(4,0),g(5,0),g(6,0),g(0,2),g(1,2),g(2,2),g(3,2),g(4,2),g(5,2),g(6,2),w.idleAtFrameEnd);
    const auto& pr=w.stage[Producer];
    const double prn=pr.joinN?pr.joinN:1;
    auto h=[&](unsigned grp,unsigned k){return Ms(pr.join[grp][k])/prn;};
    const auto& q=w.queue;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0208 producer-in state=%s n=%u bad=%u idle=(%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f) "
        "decode=(%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f) fence=(%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f) "
        "queue=(%u,%u,%u,%u,%u,%u) backlog=%.0f wrapped=%u kill=1",
        kStateName[state],pr.joinN,pr.joinBad,h(0,1),h(1,1),h(2,1),h(3,1),h(4,1),h(5,1),h(6,1),
        h(0,0),h(1,0),h(2,0),h(3,0),h(4,0),h(5,0),h(6,0),h(0,2),h(1,2),h(2,2),h(3,2),h(4,2),h(5,2),h(6,2),
        q.samples,q.consumerIdleEmpty,q.consumerIdleQueued,q.producerIdle,q.bothIdle,q.producerBusy,
        q.fendN?static_cast<double>(q.fendBacklog)/q.fendN:0.0,q.wrapped);
    auto& p=w.post;
    const unsigned pn=p.n?p.n:1;
    std::uint64_t post[300],wake[300],postSum=0,wakeSum=0;
    std::copy(p.post,p.post+p.n,post);std::copy(p.wake,p.wake+p.n,wake);
    std::sort(post,post+p.n);std::sort(wake,wake+p.n);
    for(unsigned i=0;i<p.n;++i){postSum+=post[i];wakeSum+=wake[i];}
    auto pct=[&](const std::uint64_t* v,unsigned qq){return p.n?Ms(v[(p.n*qq)/100]):0.0;};
    const double pj=p.joinN[Consumer]?p.joinN[Consumer]:1,pp=p.joinN[Producer]?p.joinN[Producer]:1;
    const auto& cb=p.before[Consumer];const auto& ca=p.after[Consumer];
    const auto& pb=p.before[Producer];const auto& pa=p.after[Producer];
    const auto& cph=p.phase[Consumer];const auto& pph=p.phase[Producer];
    Logging.Log("[GalaxyCoop:smg1] DIAG-0208 drawdone state=%s n=%u none=%u multi=%u outside=%u torn=%u posts=%u tok=%u "
        "post=(%.2f p50=%.2f p95=%.2f max=%.2f) wake=(%.2f p50=%.2f p95=%.2f max=%.2f) join=%u/%u "
        "before=(%.2f,%.2f,%.2f) after=(%.2f,%.2f,%.2f) phase=(%u,%u,%u,%u,%u,%u) thr=(main=%u,worker=%u,other=%u,unk=%u) "
        "lr=(%lx:%u,%lx:%u,%lx:%u,%lx:%u,+%u) pjoin=%u/%u pbefore=(%.2f,%.2f,%.2f) pafter=(%.2f,%.2f,%.2f) "
        "pphase=(%u,%u,%u,%u,%u,%u) kill=1",
        kStateName[state],p.n,p.none,p.multi,p.outside,p.torn,sDoneCount.load(std::memory_order_relaxed)-p.done0,
        sTokenCount.load(std::memory_order_relaxed)-p.token0,
        Ms(postSum)/pn,pct(post,50),pct(post,95),p.n?Ms(post[p.n-1]):0.0,
        Ms(wakeSum)/pn,pct(wake,50),pct(wake,95),p.n?Ms(wake[p.n-1]):0.0,p.joinN[Consumer],p.joinBad[Consumer],
        Ms(cb[1])/pj,Ms(cb[0])/pj,Ms(cb[2])/pj,Ms(ca[1])/pj,Ms(ca[0])/pj,Ms(ca[2])/pj,
        cph[0],cph[1],cph[2],cph[3],cph[4],cph[5],p.thrMain,p.thrWorker,p.thrOther,p.thrUnknown,
        p.lr[0],p.lrCount[0],p.lr[1],p.lrCount[1],p.lr[2],p.lrCount[2],p.lr[3],p.lrCount[3],p.lrOther,
        p.joinN[Producer],p.joinBad[Producer],Ms(pb[1])/pp,Ms(pb[0])/pp,Ms(pb[2])/pp,Ms(pa[1])/pp,Ms(pa[0])/pp,Ms(pa[2])/pp,
        pph[0],pph[1],pph[2],pph[3],pph[4],pph[5]);
    w=Window{};
    sLines+=6;sStateLines[state]+=6;sLogged=true;
}
// The draw-done post this frame end waited for, taken at fend exit.
void SamplePost(PostWindow& p){
    if(!p.n&&!p.none&&!p.torn&&!p.outside){
        p.done0=sDoneCount.load(std::memory_order_relaxed);p.token0=sTokenCount.load(std::memory_order_relaxed);
    }
    const unsigned posts=(sPostSeqOut-sPostSeqIn)/2;
    if(sPostTorn){++p.torn;return;}
    if(!posts){++p.none;return;}
    if(posts>1)++p.multi;
    const auto& r=sPostLocal;
    if(r.tick<sIn[FrameEnd]||r.tick>sOut[FrameEnd]){++p.outside;return;}
    p.post[p.n]=r.tick-sIn[FrameEnd];p.wake[p.n]=sOut[FrameEnd]-r.tick;++p.n;
    for(unsigned t=0;t<StageCount;++t){
        std::uint64_t before[3],after[3];
        if(StageDelta(sSnap[kSnapIn[FrameEnd]].t[t],r.snap.t[t],before)&&StageDelta(r.snap.t[t],sSnap[kSnapOut[FrameEnd]].t[t],after)){
            ++p.joinN[t];
            for(unsigned k=0;k<3;++k){p.before[t][k]+=before[k];p.after[t][k]+=after[k];}
        }else ++p.joinBad[t];
        ++p.phase[t][r.snap.t[t].ok?std::min(r.snap.t[t].phase,4u):5];
    }
    const auto worker=__atomic_load_n(&sWorkerThread,__ATOMIC_RELAXED);
    if(r.thread==sThread)++p.thrMain;
    else if(!worker)++p.thrUnknown;
    else if(r.thread==worker)++p.thrWorker;
    else ++p.thrOther;
    const auto main=Main();
    const std::uint64_t site=r.lr>=main&&r.lr<main+0x2000000?r.lr-main:r.lr;
    for(unsigned i=0;i<4;++i){
        if(p.lrCount[i]&&p.lr[i]!=site)continue;
        p.lr[i]=site;++p.lrCount[i];return;
    }
    ++p.lrOther;
}
void SampleQueue(QueueWindow& q){
    for(unsigned i=0;i<=SegmentCount;++i){
        const auto& s=sSnap[i];
        if(!s.queueOk||!s.t[Consumer].ok||!s.t[Producer].ok)continue;
        ++q.samples;
        const bool consumerIdle=s.t[Consumer].phase==2,producerIdle=s.t[Producer].phase==2;
        if(consumerIdle){if(s.read==s.write)++q.consumerIdleEmpty;else ++q.consumerIdleQueued;}
        if(producerIdle)++q.producerIdle;
        if(consumerIdle&&producerIdle)++q.bothIdle;
        if(consumerIdle&&s.t[Producer].phase==1)++q.producerBusy;
    }
    const auto& f=sSnap[kSnapIn[FrameEnd]];
    if(!f.queueOk)return;
    if(f.write>=f.read){q.fendBacklog+=f.write-f.read;++q.fendN;}else ++q.wrapped;
}
void Finish(std::uint64_t now){
    sSnap[SegmentCount]=Snap(now);
    unsigned state=NoScene;
    if(sScenes)state=!sAvailable?NonCoop:sActive?(sReveal>=1.0f?Split:Trans):Merged;
    const unsigned category=state!=sPrevState?Trans:state;
    sPrevState=state;
    sSplitFrame.store(category==Split,std::memory_order_relaxed);
    auto& w=sWindow[category];
    for(unsigned t=0;t<StageCount;++t)SampleStage(w.stage[t],t);
    bool ordered=sMask==kAllPhases&&!sRepeat&&sScenes<=1;
    for(unsigned p=0;ordered&&p<PhaseCount;++p)
        ordered=sOut[p]>=sIn[p]&&sIn[p]>=(p?sOut[p-1]:sFrameStart);
    ordered=ordered&&now>=sOut[Pace];
    if(!sValid||sLogged||!ordered){++w.skipped;return;}
    const std::uint64_t seg[SegmentCount]={
        sIn[Begin]-sFrameStart,sOut[Begin]-sIn[Begin],sIn[Draw]-sOut[Begin],sOut[Draw]-sIn[Draw],
        sIn[Calc]-sOut[Draw],sOut[Calc]-sIn[Calc],sIn[FrameEnd]-sOut[Calc],sOut[FrameEnd]-sIn[FrameEnd],
        sIn[Pace]-sOut[FrameEnd],sOut[Pace]-sIn[Pace],now-sOut[Pace]};
    for(unsigned s=0;s<SegmentCount;++s){w.sum[s]+=seg[s];w.maximum[s]=std::max(w.maximum[s],seg[s]);}
    for(unsigned t=0;t<StageCount;++t){
        auto& st=w.stage[t];
        std::uint64_t join[GroupCount][3]{};
        bool joined=true;
        for(unsigned s=0;joined&&s<SegmentCount;++s){
            std::uint64_t d[3];
            joined=StageDelta(sSnap[s].t[t],sSnap[s+1].t[t],d);
            for(unsigned k=0;joined&&k<3;++k)join[kGroupOf[s]][k]+=d[k];
        }
        if(joined){
            ++st.joinN;
            for(unsigned grp=0;grp<GroupCount;++grp)for(unsigned k=0;k<3;++k)st.join[grp][k]+=join[grp][k];
            if(t==Consumer&&sSnap[kSnapIn[FrameEnd]].t[t].phase==2)++w.idleAtFrameEnd;
        }else ++st.joinBad;
    }
    SampleQueue(w.queue);
    SamplePost(w.post);
    w.intervals[w.n++]=now-sFrameStart;
    const bool idle=category==NonCoop||category==NoScene;
    const bool budget=idle?sStateLines[category]<kMaxIdleStateLines:
        sLines-sStateLines[NonCoop]-sStateLines[NoScene]<kMaxLines;
    if(w.n>=kWindow[category]&&budget)Flush(category);
    else if(w.n>=kWindow[category])w=Window{};
}
bool PhaseOwned(){if(Thread()==sThread)return true;++sForeign;return false;}
std::uint64_t PhaseStart(unsigned p){
    if(p==FrameEnd)sPostSeqIn=sPostSeq.load(std::memory_order_acquire);
    const auto now=Clock();
    if(kSnapIn[p]!=kNoSnap)sSnap[kSnapIn[p]]=Snap(now);
    return now;
}
void PhaseStop(unsigned p,std::uint64_t start){
    if(sMask&(1u<<p))++sRepeat;
    const auto now=Clock();
    sIn[p]=start;sOut[p]=now;sMask|=1u<<p;
    if(kSnapOut[p]!=kNoSnap)sSnap[kSnapOut[p]]=Snap(now);
    if(p==FrameEnd){
        const auto first=sPostSeq.load(std::memory_order_acquire);
        sPostLocal=sPost;
        std::atomic_thread_fence(std::memory_order_acquire);
        sPostSeqOut=first;sPostTorn=(first&1)||sPostSeq.load(std::memory_order_relaxed)!=first;
    }
}
#define GALAXY_TIMELINE_PHASE(Name,Index) \
HOOK_DEFINE_TRAMPOLINE(Name){ \
    static std::uintptr_t Callback(std::uintptr_t a,std::uintptr_t b,std::uintptr_t c,std::uintptr_t d){ \
        if(!PhaseOwned())return Orig(a,b,c,d); \
        const auto start=PhaseStart(Index); \
        const auto result=Orig(a,b,c,d); \
        PhaseStop(Index,start); \
        return result; \
    } \
};
GALAXY_TIMELINE_PHASE(TimelineBegin,Begin)
GALAXY_TIMELINE_PHASE(TimelineDraw,Draw)
GALAXY_TIMELINE_PHASE(TimelineEnd,End)
GALAXY_TIMELINE_PHASE(TimelineCalc,Calc)
GALAXY_TIMELINE_PHASE(TimelineFrameEnd,FrameEnd)
GALAXY_TIMELINE_PHASE(TimelinePace,Pace)
#undef GALAXY_TIMELINE_PHASE
HOOK_DEFINE_TRAMPOLINE(TimelineLoop){
    static std::uintptr_t Callback(std::uintptr_t a,std::uintptr_t b,std::uintptr_t c,std::uintptr_t d){
        const auto now=Clock();
        if(!sThread){sThread=Thread();asm volatile("mrs %0,cntfrq_el0":"=r"(sFrequency));}
        if(Thread()!=sThread||!sFrequency){++sForeign;return Orig(a,b,c,d);}
        if(sFrameStart)Finish(now);
        sFrameId.fetch_add(1,std::memory_order_relaxed);
        std::uintptr_t timers[StageCount]{},context=0;
        const bool anchored=Anchor(timers,context);
        for(unsigned t=0;t<StageCount;++t)__atomic_store_n(&sTimer[t],anchored?timers[t]:0,__ATOMIC_RELAXED);
        __atomic_store_n(&sContext,anchored?context:0,__ATOMIC_RELAXED);
        sSnap[0]=Snap(now);
        // A log line costs this frame, so it is skipped next time round.
        sValid=!sLogged;sLogged=false;
        sFrameStart=now;sMask=sRepeat=sScenes=0;sActive=sAvailable=false;sReveal=0;
        return Orig(a,b,c,d);
    }
};
// Runs on whichever thread raises the PE event; w1==1 is draw done.
HOOK_DEFINE_TRAMPOLINE(TimelinePePost){
    static std::uintptr_t Callback(std::uintptr_t a,std::uintptr_t b,std::uintptr_t c,std::uintptr_t d){
        // 526BC tail-branches here, so x30 is the slot caller.
        const auto lr=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const auto type=static_cast<std::uint32_t>(b);
        if(type==1&&sFrequency){
            const auto now=Clock();
            const auto seq=sPostSeq.load(std::memory_order_relaxed);
            sPostSeq.store(seq+1,std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            sPost.tick=now;sPost.thread=Thread();sPost.lr=lr;sPost.snap=Snap(now);
            sPostSeq.store(seq+2,std::memory_order_release);
            sDoneCount.fetch_add(1,std::memory_order_relaxed);
        }else if(!type)sTokenCount.fetch_add(1,std::memory_order_relaxed);
        return Orig(a,b,c,d);
    }
};
// C86C runs on the thread owning the timer: 4574 consumer or 5048 producer.
HOOK_DEFINE_TRAMPOLINE(TimelinePublish){
    static std::uintptr_t Callback(std::uintptr_t a,std::uintptr_t b,std::uintptr_t c,std::uintptr_t d){
        if(a&&a==__atomic_load_n(&sTimer[Consumer],__ATOMIC_RELAXED))__atomic_store_n(&sWorkerThread,Thread(),__ATOMIC_RELAXED);
        else if(a&&a==__atomic_load_n(&sTimer[Producer],__ATOMIC_RELAXED))__atomic_store_n(&sProducerThread,Thread(),__ATOMIC_RELAXED);
        return Orig(a,b,c,d);
    }
};
}
std::uint32_t FrameTimelineFrameId(){return sFrameId.load(std::memory_order_relaxed);}
bool FrameTimelineSplit(){return sSplitFrame.load(std::memory_order_relaxed);}
std::uintptr_t FrameTimelineProducerThread(){return __atomic_load_n(&sProducerThread,__ATOMIC_RELAXED);}
void FrameTimelineSceneState(bool active,float reveal,bool available){
    if(Thread()!=sThread)return;
    ++sScenes;sActive=active;sReveal=reveal;sAvailable=available;
}
void InstallFrameTimeline(){
    TimelineLoop::InstallAtOffset(kLoop);
    TimelineBegin::InstallAtOffset(kBegin);TimelineDraw::InstallAtOffset(kDraw);TimelineEnd::InstallAtOffset(kEnd);
    TimelineCalc::InstallAtOffset(kCalc);TimelineFrameEnd::InstallAtOffset(kFrameEnd);TimelinePace::InstallAtOffset(kPace);
    TimelinePePost::InstallAtOffset(kPePost);TimelinePublish::InstallAtOffset(kPublish);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0208 ATT-0191 frame timeline v5 installed loop=0x97790 phases=98F20/98154/9842C/97A2C/97BF0/978E4 "
        "worker=DA8+68 producer=owner+4F2CB8 queue=owner+3B0+1B3C/1B40 drawdone=526C8 publish=C86C window=300 forward-all kill=1");
}
}
#endif
