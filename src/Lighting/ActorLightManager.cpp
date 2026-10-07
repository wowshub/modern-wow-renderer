#include "ActorLightManager.h"
#include "ActorLightMath.h"
#include "../Core/FrameContext.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <cstring>
namespace renderer {
namespace {
// Verified against blacknight Wow.exe SHA-256 in Configure. Read on render thread.
// No remote injection, game memory writes, packet changes or automation.
bool ReadBytes(uintptr_t a,void* out,size_t n) {
 if(a<0x10000||n>4096||a>UINTPTR_MAX-n)return false;
 __try {std::memcpy(out,reinterpret_cast<void*>(a),n);return true;}
 __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
template<class T> bool Read(uintptr_t a,T& out){return ReadBytes(a,&out,sizeof(out));}
bool Position(uintptr_t base,uintptr_t object,Vec3& out) {
 // CGUnit_C::GetPosition resolves transport-local positions to world coordinates.
 __try {reinterpret_cast<void*(__thiscall*)(void*,Vec3*)>(base+0x2e6ef0)(reinterpret_cast<void*>(object),&out);}
 __except(EXCEPTION_EXECUTE_HANDLER){return false;}
 return std::isfinite(out.x)&&std::isfinite(out.y)&&std::isfinite(out.z)&&std::abs(out.x)<100000&&std::abs(out.y)<100000&&std::abs(out.z)<100000;
}
bool Indoors(uintptr_t base,uintptr_t object,bool& result) {
 // Exact function called by native Lua IsIndoors; queries WMO/terrain outdoors.
 __try {result=!reinterpret_cast<bool(__thiscall*)(void*)>(base+0x31b7f0)(reinterpret_cast<void*>(object));return true;}
 __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
std::wstring Text(const std::wstring& path,const wchar_t* section,const wchar_t* key,const wchar_t* fallback=L"") {wchar_t v[512]{};GetPrivateProfileStringW(section,key,fallback,v,512,path.c_str());return v;}
float Number(const std::wstring& path,const wchar_t* section,const wchar_t* key,float fallback,float lo,float hi){auto v=Text(path,section,key);wchar_t* end=nullptr;float f=wcstof(v.c_str(),&end);return end==v.c_str()||*end||!std::isfinite(f)?fallback:std::clamp(f,lo,hi);}
Vec3 Color(const std::wstring& path,const wchar_t* section,Vec3 fallback){auto str=Text(path,section,L"Color");std::wistringstream in(str);Vec3 c;wchar_t a,b;if(!(in>>c.x>>a>>c.y>>b>>c.z)||a!=L','||b!=L','||!std::isfinite(c.x)||!std::isfinite(c.y)||!std::isfinite(c.z))return fallback;return {std::clamp(c.x,0.f,1.f),std::clamp(c.y,0.f,1.f),std::clamp(c.z,0.f,1.f)};}
std::vector<std::wstring> Section(const std::wstring& path,const wchar_t* name){std::vector<wchar_t> buf(32768);GetPrivateProfileSectionW(name,buf.data(),DWORD(buf.size()),path.c_str());std::vector<std::wstring> result;for(const wchar_t* p=buf.data();*p;p+=wcslen(p)+1)result.emplace_back(p);return result;}
bool InView(const Vec3& position,float radius){const auto& f=FrameContext::Current();auto d=position-f.cameraPosition;float z=Dot(d,{f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2]});if(z < -radius)return false;float x=Dot(d,{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2]});float y=Dot(d,{f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2]});return std::abs(x)*std::abs(f.projUnpack[2])<=std::max(0.f,z)+radius*2&&std::abs(y)*std::abs(f.projUnpack[3])<=std::max(0.f,z)+radius*2;}
}
ActorLightManager& ActorLightManager::Instance(){static ActorLightManager m;return m;}
void ActorLightManager::Log(const std::string& v){std::ofstream f(std::filesystem::path(log),std::ios::app);f<<v<<'\n';}
void ActorLightManager::Configure(const std::wstring& path){ini=path+L"GraphicsEffects.ini";log=path+L"ActorLighting.log";client.Detect();supported=client.sha256=="2d89cf4231fa27b6f4f9a5e1ba08c6473474d04d62421725eb99c50f34dd99c2";
 // Also verify the entry bytes: memory patching of these functions invalidates this provider.
 unsigned char bytes[9]{};const unsigned char expected[]={0x55,0x8b,0xec,0x8b,0x89,0xd8,0,0,0};
 supported=supported&&client.Read(0x2e6ef0,bytes,sizeof(bytes))&&std::memcmp(bytes,expected,sizeof(bytes))==0;
 Log("LIGHT2A exe="+client.sha256+(supported?" provider enabled (candidate)":" unsupported; actor lights disabled"));Reload();}
void ActorLightManager::Reload(){
 auto settings=[&](Settings& s,const wchar_t* name,bool isPlayer){s.enabled=Number(ini,name,L"Enabled",1,0,1)!=0;s.radius=Number(ini,name,isPlayer?L"BaseRadius":L"Radius",isPlayer?3.f:1.8f,.2f,40);s.intensity=Number(ini,name,isPlayer?L"BaseIntensity":L"Intensity",isPlayer?.35f:.08f,0,3);s.height=Number(ini,name,L"HeightOffset",.8f,.1f,4);s.color=Color(ini,name,isPlayer?Vec3{1,.72f,.4f}:Vec3{.85f,.75f,.6f});s.day=Number(ini,name,L"OutdoorDayMultiplier",isPlayer?.15f:.05f,0,2);s.night=Number(ini,name,L"OutdoorNightMultiplier",1,0,2);s.interior=Number(ini,name,L"DarkInteriorMultiplier",1,0,2);s.transition=Number(ini,name,L"TransitionSeconds",.8f,.05f,10);};
 settings(player,L"PlayerLight",true);settings(creature,L"CreatureLight",false);
 distance=Number(ini,L"CreatureLight",L"MaxDistance",35,1,100);maxLights=int(Number(ini,L"CreatureLight",L"MaxActiveLights",16,0,23));fadeDeath=Number(ini,L"CreatureLight",L"FadeOnDeath",1,0,1)!=0;deathSeconds=Number(ini,L"CreatureLight",L"DeathFadeSeconds",.8f,.05f,5);
 auto mode=Text(ini,L"PlayerLight",L"EquipmentMode",L"Strongest");additiveEquipment=_wcsicmp(mode.c_str(),L"Additive")==0;
 additionalRadiusFactor=Number(ini,L"PlayerLight",L"AdditionalRadiusFactor",.5f,0,1);maxEquipmentRadius=Number(ini,L"PlayerLight",L"MaxEquipmentRadius",40,.2f,40);
 forceProfile=Text(ini,L"PlayerLight",L"TestProfile");daylightOverride=Number(ini,L"PlayerLight",L"TestDaylight",-1,-1,1);interiorOverride=int(Number(ini,L"PlayerLight",L"TestIndoors",-1,-1,1));
 profiles.clear();items.clear();areaOverrides.clear();std::vector<wchar_t> names(32768);GetPrivateProfileSectionNamesW(names.data(),DWORD(names.size()),ini.c_str());
 for(const wchar_t* n=names.data();*n;n+=wcslen(n)+1){std::wstring name=n;if(name.rfind(L"LightProfile.",0)!=0)continue;Profile p;p.radius=Number(ini,n,L"Radius",8,.2f,40);p.intensity=Number(ini,n,L"Intensity",1.1f,0,3);p.color=Color(ini,n,p.color);p.flicker=Number(ini,n,L"FlickerAmount",0,0,.2f);profiles[name.substr(13)]=p;}
 for(auto row:Section(ini,L"EquipmentLights")){auto eq=row.find(L'=');if(eq==std::wstring::npos)continue;auto key=row.substr(0,eq);wchar_t* end=nullptr;auto id=wcstoul(key.c_str(),&end,10);auto profile=row.substr(eq+1);if(id&&end!=key.c_str()&&!*end&&profiles.count(profile))items[uint32_t(id)]=profile;}
 for(auto row:Section(ini,L"DarkInteriorAreas")){auto eq=row.find(L'=');if(eq==std::wstring::npos)continue;auto key=row.substr(0,eq);wchar_t* end=nullptr;auto id=wcstoul(key.c_str(),&end,10);if(id&&end!=key.c_str()&&!*end)areaOverrides[uint32_t(id)]=row.substr(eq+1)==L"1";}
 Log("config reloaded; equipment mappings="+std::to_string(items.size())+" creature cap="+std::to_string(maxLights));
}
void ActorLightManager::Reset(){states.clear();actors.clear();valid=false;equipmentActive=false;sampled=last=0;sessionGuid=0;intensity=0;}
void ActorLightManager::Sample(){
 valid=false;actors.clear();equipment.fill(0);uintptr_t conn=0,mgr=0,obj=0;uint32_t link=0;uint64_t guid=0;
 if(!supported||!client.Read(0x879ce0,&conn,4)||!Read(conn+0x2ed0,mgr)||!Read(mgr+0xc0,guid)||!guid||!Read(mgr+0xac,obj)||!Read(mgr+0xa4,link)||link<0x20||link>0x100)return;
 uint32_t map=0;client.Read(0x7d088c,&map,4);if(guid!=sessionGuid||map!=mapId){states.clear();intensity=0;sessionGuid=guid;mapId=map;}
 std::set<uintptr_t> visited;uintptr_t selfPtr=0;std::vector<Actor> collected;
 for(unsigned i=0;i<4096&&obj&&!(obj&1);++i){if(!visited.insert(obj).second)break;uintptr_t next=0,desc=0,vt=0,fn=0;uint32_t type=0;uint64_t id=0;
  if(!Read(obj+link+4,next)||!Read(obj+0x14,type))break;
  if((type==3||type==4)&&Read(obj+8,desc)&&Read(desc,id)&&id&&Read(obj,vt)&&Read(vt+0x2c,fn)&&fn==client.base+0x2e6ef0){
   Actor a;a.guid=id;uint32_t hp=0,maxhp=0;uint64_t again=0;
   if(Read(desc+0x60,hp)&&Read(desc+0x80,maxhp)&&maxhp&&hp<=maxhp&&Position(client.base,obj,a.pos)&&Read(obj+0x30,again)&&again==id){a.alive=hp>0;
    if(id==guid){self=a;selfPtr=obj;uint32_t visible[38]{};if(ReadBytes(desc+0x46c,visible,sizeof(visible)))for(unsigned slot=0;slot<19;++slot)equipment[slot]=visible[slot*2];}
    else if(type==3)collected.push_back(a);
   }
  }
  obj=next;
 }
 uint64_t again=0;uintptr_t mgrAgain=0;if(!selfPtr||!Read(conn+0x2ed0,mgrAgain)||mgrAgain!=mgr||!Read(mgr+0xc0,again)||again!=guid)return;
 if(!Indoors(client.base,selfPtr,indoor))return;
 uint32_t area=0;client.Read(0x7d0810,&area,4);if(areaOverrides.count(area))indoor=areaOverrides[area];if(interiorOverride>=0)indoor=interiorOverride!=0;
 for(auto a:collected)if(Length(a.pos-self.pos)<=distance)actors.push_back(a);
 valid=true;
}
bool ActorLightManager::SuppressNative(const LocalLightSource& l)const{return valid&&equipmentActive&&(l.flags&LocalLightAttached)&&Length(l.position-self.pos)<3.0f;}
void ActorLightManager::Append(std::vector<LocalLightSource>& lights){
 if(!supported||(!player.enabled&&!creature.enabled)){equipmentActive=false;states.clear();return;}auto now=GetTickCount64();float dt=last?std::clamp(float(now-last)*.001f,0.f,.25f):.016f;last=now;
 if(now-sampled>=50||!sampled){Sample();sampled=now;}
 if(!valid){states.clear();equipmentActive=false;intensity=0;if(now-lastLog>5000){lastLog=now;Log("No valid in-world player snapshot; actor lights skipped");}return;}
 const auto& frame=FrameContext::Current();float daylight=daylightOverride>=0?daylightOverride:frame.daylightFactor;
 Profile target;target.radius=player.radius;target.intensity=player.intensity;target.color=player.color;bool equipped=false;
 std::vector<Profile> equippedProfiles;std::vector<actorlight::EquipmentLight> powers;
 for(uint32_t id:equipment){auto m=items.find(id);if(m==items.end())continue;auto p=profiles.find(m->second);if(p==profiles.end())continue;equippedProfiles.push_back(p->second);powers.push_back({p->second.radius,p->second.intensity});}
 auto combined=actorlight::Combine(powers,additiveEquipment,additionalRadiusFactor,maxEquipmentRadius);
 if(combined.count){target=equippedProfiles[combined.strongest];target.radius=combined.radius;equipped=true;}

 if(auto f=profiles.find(forceProfile);f!=profiles.end()){target=f->second;equipped=true;}
 equipmentActive=equipped&&player.enabled;
 radius=actorlight::Blend(radius,target.radius,dt,player.transition);intensity=actorlight::Blend(intensity,self.alive?target.intensity:0.f,dt,player.transition);color=color+(target.color-color)*(1-std::exp(-dt/player.transition));environment=actorlight::Blend(environment,actorlight::Environment(daylight,indoor,player.day,player.night,player.interior),dt,player.transition);
 if(player.enabled&&intensity>.0001f){LocalLightSource l;l.stableId=self.guid^0x504c000000000000ull;l.position=self.pos;l.position.z+=player.height;l.radius=radius;l.color=color;l.intensity=intensity*environment*(1+target.flicker*std::sin(float(now%100000)*.017f));l.flags=LocalLightPoint|LocalLightPlayer;l.score=1000000;lights.push_back(l);}
 if(creature.enabled){for(auto a:actors){if(!a.alive)continue;auto& st=states[a.guid];st.seen=now;st.light.stableId=a.guid;st.light.position=a.pos;st.light.position.z+=creature.height;st.light.radius=creature.radius;st.light.color=creature.color;st.light.flags=LocalLightPoint|LocalLightCreature;}
  std::vector<LocalLightSource> nearest;float env=actorlight::Environment(daylight,indoor,creature.day,creature.night,creature.interior);
  for(auto it=states.begin();it!=states.end();){auto& st=it->second;float age=float(now-st.seen)*.001f;if(age>deathSeconds||(!fadeDeath&&age>0)||Length(st.light.position-self.pos)>distance+2){it=states.erase(it);continue;}auto l=st.light;l.intensity=creature.intensity*env*(fadeDeath?std::clamp(1-age/deathSeconds,0.f,1.f):1.f);l.score=1.f/(1.f+Length(l.position-frame.cameraPosition));if(l.intensity>.0001f&&InView(l.position,l.radius))nearest.push_back(l);++it;}
  std::sort(nearest.begin(),nearest.end(),[](const auto& a,const auto& b){return a.score>b.score;});if(nearest.size()>size_t(maxLights))nearest.resize(maxLights);lights.insert(lights.end(),nearest.begin(),nearest.end());
 }else states.clear();
 if(now-lastLog>5000){lastLog=now;std::ostringstream o;o<<"valid player="<<self.guid<<" pos="<<self.pos.x<<","<<self.pos.y<<","<<self.pos.z<<" indoors="<<indoor<<" daylight="<<daylight<<" radius="<<radius<<" intensity="<<intensity*environment<<" creatures="<<actors.size()<<" equipped="<<equipped<<" equipmentCount="<<combined.count<<" mode="<<(additiveEquipment?"Additive":"Strongest")<<" targetRadius="<<target.radius<<" itemIDs=";for(auto id:equipment)if(id)o<<id<<",";Log(o.str());}
}
}
