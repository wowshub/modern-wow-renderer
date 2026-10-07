#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstddef>
namespace renderer::actorlight {
inline float Blend(float current,float target,float dt,float seconds) { return current+(target-current)*(1.f-std::exp(-std::max(0.f,dt)/std::max(.01f,seconds))); }
inline float Environment(float daylight,bool indoors,float day,float night,float interior) { return indoors?interior:night+(day-night)*std::clamp(daylight,0.f,1.f); }
inline float Strength(float radius,float intensity) { return radius*radius*intensity; }
struct EquipmentLight { float radius, intensity; };
struct EquipmentResult { size_t strongest=0; float radius=0; size_t count=0; };
inline EquipmentResult Combine(const std::vector<EquipmentLight>& lights,bool additive,float fraction,float cap) {
 EquipmentResult result;result.count=lights.size();if(lights.empty())return result;
 float score=-1,sum=0,largest=0;
 for(size_t i=0;i<lights.size();++i){const auto& l=lights[i];float candidate=Strength(l.radius,l.intensity);
  if(candidate>score){score=candidate;result.strongest=i;}sum+=l.radius;largest=std::max(largest,l.radius);
 }
 // Preserve legacy Strongest behavior. Additive affects range only, not intensity/color.
 result.radius=additive?std::min(std::clamp(cap,.2f,40.f),largest+std::max(0.f,sum-largest)*std::clamp(fraction,0.f,1.f)):lights[result.strongest].radius;
 return result;
}
}
