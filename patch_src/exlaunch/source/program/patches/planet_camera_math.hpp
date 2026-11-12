#pragma once
#include <algorithm>
#include <cmath>

namespace mod::patches::planet {
struct Vec {
    float x,y,z;
    Vec operator+(Vec b) const {return {x+b.x,y+b.y,z+b.z};}
    Vec operator-(Vec b) const {return {x-b.x,y-b.y,z-b.z};}
    Vec operator*(float s) const {return {x*s,y*s,z*s};}
};
inline float Dot(Vec a,Vec b) {return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec Cross(Vec a,Vec b) {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline float Length(Vec v) {return std::sqrt(Dot(v,v));}
inline bool Finite(Vec v) {return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&Length(v)<1.0e7f;}
inline Vec Unit(Vec v,Vec fallback={0,1,0}) {const float n=Length(v);return Finite(v)&&n>0.0001f ? v*(1/n) : fallback;}
inline Vec Perpendicular(Vec up) {return Unit(Cross(up,std::abs(up.x)<0.7f ? Vec{1,0,0}:Vec{0,0,1}));}
inline Vec Rotate(Vec v,Vec axis,float angle) {
    const float c=std::cos(angle),s=std::sin(angle);
    return v*c+Cross(axis,v)*s+axis*(Dot(axis,v)*(1-c));
}
inline Vec Transport(Vec v,Vec from,Vec to) {
    const float c=std::clamp(Dot(from,to),-1.0f,1.0f);
    const Vec cross=Cross(from,to);
    const float sine=Length(cross);
    if (sine<0.0001f) return c>0 ? v : Rotate(v,Perpendicular(from),3.14159265f);
    return Rotate(v,cross*(1/sine),std::atan2(sine,c));
}
inline Vec StepDirection(Vec from,Vec to,float maxAngle) {
    const float angle=std::acos(std::clamp(Dot(from,to),-1.0f,1.0f));
    if (angle<0.0001f) return to;
    Vec axis=Unit(Cross(from,to),Perpendicular(from));
    return Unit(Rotate(from,axis,std::min(angle,maxAngle)),to);
}
inline float LeaderWeight(float upDot) {return 0.5f+0.5f*std::clamp(0.5f-upDot,0.0f,1.0f);}
inline Vec SharedUp(Vec a,Vec b,int leader) {
    const float w=LeaderWeight(Dot(a,b));
    return Unit((leader==0?a:b)*w+(leader==0?b:a)*(1-w),leader==0?a:b);
}
// Fit only a plausible common radial surface. Flat or unrelated gravity
// regions retain the weighted player pivot instead of inventing a center.
inline bool SurfacePivot(const Vec points[2],const Vec ups[2],Vec sharedUp,Vec& pivot) {
    const Vec du=ups[0]-ups[1],dp=points[0]-points[1];
    const float denominator=Dot(du,du);
    if (denominator<0.03f) return false;
    const float radius=Dot(dp,du)/denominator;
    if (!std::isfinite(radius)||radius<100||radius>10000 ||
        Length(dp-du*radius)>std::max(100.0f,Length(dp)*0.25f)) return false;
    const Vec center=(points[0]+points[1]-(ups[0]+ups[1])*radius)*0.5f;
    pivot=center+sharedUp*radius;
    return Finite(pivot);
}
inline float FitDistance(const Vec points[2],Vec watch,Vec outward,Vec up,float fovy,float minimum) {
    const Vec forward=outward*-1;
    const Vec right=Unit(Cross(forward,up),Perpendicular(up));
    const Vec screenUp=Cross(right,forward);
    const float vertical=std::tan(fovy*0.00872664626f)*0.8f;
    float distance=minimum;
    for(int i=0;i<2;++i) {
        const Vec r=points[i]-watch;
        distance=std::max(distance,(std::abs(Dot(r,right))+90)/(vertical*16/9)-Dot(r,forward));
        distance=std::max(distance,(std::abs(Dot(r,screenUp))+90)/vertical-Dot(r,forward));
    }
    return std::clamp(distance,minimum,4500.0f);
}
}
