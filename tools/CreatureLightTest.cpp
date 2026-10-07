#include "../src/Lighting/ActorLightMath.h"
#include <map>
#include <string>
#include <cassert>
#include <cstdio>
struct S { bool enabled;float radius,intensity; };
int main(){std::map<unsigned,std::string> m{{100,"Boss"},{200,"Off"},{300,"Missing"}};std::map<std::string,S> p{{"Boss",{true,6,.8f}},{"Off",{false,3,0}}};S global{true,3,.3f};using renderer::actorlight::ResolveProfile;
 assert(&ResolveProfile(999u,m,p,global)==&global);assert(ResolveProfile(100u,m,p,global).radius==6);assert(!ResolveProfile(200u,m,p,global).enabled);assert(&ResolveProfile(300u,m,p,global)==&global);
 p["Boss"].radius=12;assert(ResolveProfile(100u,m,p,global).radius==12);m.erase(100);assert(&ResolveProfile(100u,m,p,global)==&global);
 for(unsigned id=1000;id<11000;++id)assert(&ResolveProfile(id,m,p,global)==&global);
 puts("PASS explicit override, disable, missing profile fallback, reload/removal and 10000 unmapped entries");}
