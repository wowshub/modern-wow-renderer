#include "../src/Lighting/ActorLightMath.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
int main(){using namespace renderer::actorlight;
 assert(Combine({},true,.5f,40).count==0);
 assert(Combine({{8,1.1f}},true,.5f,40).radius==8);
 assert(Combine({{20,1.5f}},true,.5f,40).radius==20);
 assert(Combine({{8,1.1f},{20,1.5f}},true,.5f,40).radius==24);
 assert(Combine({{8,1.1f},{20,1.5f}},false,.5f,40).radius==20);
 assert(Combine({{8,1.1f},{20,1.5f}},true,1,40).radius==28);
 assert(Combine({{8,1.1f},{20,1.5f}},true,0,40).radius==20);
 assert(Combine({{8,1.1f},{20,1.5f}},true,1,22).radius==22);
 assert(Combine({{8,1.1f},{8,1.1f}},true,.5f,40).radius==12);
 assert(Combine({{3,3},{20,.01f}},false,.5f,40).radius==3);
 assert(Combine({{3,3},{20,.01f}},true,.5f,40).radius==21.5f);
 std::mt19937 random(2808);std::uniform_real_distribution<float>d(.2f,40.f);
 for(int n=0;n<10000;++n){std::vector<EquipmentLight> v;for(unsigned i=0;i<1+random()%19;++i)v.push_back({d(random),d(random)/14});auto a=Combine(v,true,.5f,40);float greatest=0;for(auto x:v)greatest=std::max(greatest,x.radius);assert(a.radius>=greatest&&a.radius<=40);std::reverse(v.begin(),v.end());auto b=Combine(v,true,.5f,40);assert(std::abs(a.radius-b.radius)<.0001f);}
 puts("PASS empty/single/equip/remove basis, additive/legacy, cap, duplicate slots, 10000 bounded order-independent combinations");}
