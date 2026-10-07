#pragma once
#include <algorithm>
#include <cmath>
namespace renderer::actorlight {
inline float Blend(float current,float target,float dt,float seconds) { return current+(target-current)*(1.f-std::exp(-std::max(0.f,dt)/std::max(.01f,seconds))); }
inline float Environment(float daylight,bool indoors,float day,float night,float interior) { return indoors?interior:night+(day-night)*std::clamp(daylight,0.f,1.f); }
inline float Strength(float radius,float intensity) { return radius*radius*intensity; }
}
