#include "../src/Lighting/ActorLightMath.h"
#include <cassert>
#include <cstdio>
#include <random>
int main(){using namespace renderer::actorlight;
 assert(Environment(1,false,.15f,1,1)==.15f || std::abs(Environment(1,false,.15f,1,1)-.15f)<1e-6f);
 assert(Environment(0,false,.15f,1,1)==1);assert(Environment(1,true,.15f,1,.9f)==.9f);
 assert(Environment(-1,false,.15f,1,1)==1);assert(std::abs(Environment(2,false,.15f,1,1)-.15f)<1e-6f);
 std::mt19937 rng(135);std::uniform_real_distribution<float> v(0,40),time(.01f,3);
 for(int i=0;i<10000;++i){float a=v(rng),b=v(rng),d=time(rng),tau=time(rng);float r=Blend(a,b,d,tau);assert(r>=std::min(a,b)-1e-5f&&r<=std::max(a,b)+1e-5f);float halves=Blend(Blend(a,b,d*.5f,tau),b,d*.5f,tau);assert(std::abs(r-halves)<2e-5f);assert(Blend(a,b,0,tau)==a);}
 assert(Strength(12,1.3f)>Strength(8,1.1f));puts("PASS environment overrides, strongest profile, 10000 bounded/frame-rate-independent fades");}
