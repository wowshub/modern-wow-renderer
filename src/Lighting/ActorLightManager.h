#pragma once
#include "LocalLightManager.h"
#include "../Game/WoWClientContext.h"
#include <map>
namespace renderer {
// LIGHT1A: all client data is read-only, exact-executable gated.
class ActorLightManager {
public:
 static ActorLightManager& Instance();
 void Configure(const std::wstring& path);
 void Reload();
 void Reset();
 void Append(std::vector<LocalLightSource>& lights);
 bool SuppressNative(const LocalLightSource& light) const;
private:
 struct Settings { bool enabled=true; float radius=3,intensity=.35f,height=.8f,day=.15f,night=1,interior=1,transition=.8f; Vec3 color{1,.72f,.40f}; } player,creature;
 struct Profile {float radius=8,intensity=1.1f,flicker=0;Vec3 color{1,.55f,.2f};};
 struct Actor {uint64_t guid=0;uint32_t entry=0;Vec3 pos{};bool alive=false;};
 struct State {Settings settings;LocalLightSource light;ULONGLONG seen=0;};
 std::wstring ini,log;WoWClientContext client;bool supported=false,indoor=false,valid=false,fadeDeath=true,equipmentActive=false;
 std::map<std::wstring,Profile> profiles;std::map<uint32_t,std::wstring> items;std::map<uint32_t,bool> areaOverrides;
 std::map<std::wstring,Settings> creatureProfiles;std::map<uint32_t,std::wstring> creatureMappings;
 std::map<uint64_t,State> states;std::vector<Actor> actors;Actor self;uint64_t sessionGuid=0;uint32_t mapId=0;
 std::wstring forceProfile;std::array<uint32_t,19> equipment{};
 bool additiveEquipment=false;float additionalRadiusFactor=.5f,maxEquipmentRadius=40;
 float distance=35,deathSeconds=.8f,daylightOverride=-1;int maxLights=16,interiorOverride=-1;ULONGLONG sampled=0,last=0,lastLog=0;
 float radius=3,intensity=0,environment=1;Vec3 color{1,.72f,.4f};
 void Sample();void Log(const std::string& value);
};
}
