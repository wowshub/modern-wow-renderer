#include "../Environment/LocationTuning.h"
#include "LocalLightManager.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <wrl/client.h>
#include <cstring>
#include "../Core/FrameContext.h"
#include "../D3D9/TrackedRenderState.h"
#include "../Materials/TextureHashLookup.h"
#include "TorchSignatures.h"
#include "ActorLightManager.h"

namespace renderer
{
    namespace
    {
        // Particle draws may be skipped briefly when a carried flame is close
        // to the character silhouette or crosses the frustum edge. Keep the
        // already-confirmed emitter stable across those gaps, then fade it out
        // slowly enough that unequipping a torch does not cause a hard pop.
        constexpr ULONGLONG kAttachedHoldMs = 1000;
        constexpr ULONGLONG kAttachedLifetimeMs = 3500;
    }

    LocalLightManager& LocalLightManager::Instance()
    {
        static LocalLightManager manager;
        return manager;
    }

    void LocalLightManager::Configure(const std::wstring& basePath)
    {
        m_basePath = basePath;
        m_iniPath = basePath + L"GraphicsEffects.ini";
        m_logPath = basePath + L"LocalLighting.log";
        ActorLightManager::Instance().Configure(basePath);
        ReloadTuning();
        LoadManifest();
        Log("LocalLightManager configured; native D3D9 world-light capture active");
    }

    void LocalLightManager::ReloadTuning()
    {
        ActorLightManager::Instance().Reload();
        m_enabled = renderer::locationtuning::ReadInt(L"DynamicLighting", L"Enabled", 1, m_iniPath.c_str()) != 0;
        m_intensityScale = std::clamp(int(renderer::locationtuning::ReadInt(L"DynamicLighting", L"IntensityPercent", 100, m_iniPath.c_str())), 0, 250) * 0.01f;
        m_rayScale = std::clamp(int(renderer::locationtuning::ReadInt(L"DynamicLighting", L"RayPercent", 75, m_iniPath.c_str())), 0, 200) * 0.01f;
        m_quality = std::clamp(int(renderer::locationtuning::ReadInt(L"DynamicLighting", L"Quality", 1, m_iniPath.c_str())), 0, 2);
        m_debugMode = std::clamp(int(renderer::locationtuning::ReadInt(L"DynamicLighting", L"DebugMode", 0, m_iniPath.c_str())), 0, 4);
    }

    void LocalLightManager::Reset()
    {
        ActorLightManager::Instance().Reset();
        m_nativeLights.clear();
        m_attached.clear();
        m_smoothed.clear();
        m_selected.clear();
        m_selectionSignature = 0;
    }

    void LocalLightManager::LoadManifest()
    {
        m_manifestLights.clear();
        const std::filesystem::path path = std::filesystem::path(m_basePath) / L"MaterialCache" / L"local_light_manifest.json";
        std::ifstream file(path);
        if (!file) { Log("Optional local light manifest not found; native D3D9 discovery remains active"); return; }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        size_t pos = 0;
        while ((pos = text.find("\"world_position\"", pos)) != std::string::npos)
        {
            const size_t begin = text.rfind('{', pos), end = text.find('}', pos);
            if (begin == std::string::npos || end == std::string::npos) break;
            const std::string block = text.substr(begin, end - begin + 1);
            auto array3 = [&](const char* key, Vec3 fallback) {
                const size_t k = block.find(std::string("\"") + key + "\"");
                if (k == std::string::npos) return fallback;
                const size_t b = block.find('[', k); if (b == std::string::npos) return fallback;
                std::stringstream stream(block.substr(b + 1)); Vec3 v = fallback; char comma = 0;
                if (!(stream >> v.x >> comma >> v.y >> comma >> v.z)) return fallback;
                return v;
            };
            auto number = [&](const char* key, float fallback) {
                const size_t k = block.find(std::string("\"") + key + "\"");
                if (k == std::string::npos) return fallback;
                const size_t c = block.find(':', k); if (c == std::string::npos) return fallback;
                char* tail = nullptr; const float value = std::strtof(block.c_str() + c + 1, &tail);
                return tail == block.c_str() + c + 1 ? fallback : value;
            };
            LocalLightSource light;
            light.position = array3("world_position", {});
            light.direction = Normalize(array3("direction", { 0,0,-1 }));
            light.color = array3("color", { 1,.72f,.38f });
            light.radius = std::clamp(number("radius", 12.f), 1.f, 100.f);
            light.intensity = std::clamp(number("intensity", 1.f), .01f, 8.f);
            light.confidence = std::clamp(number("confidence", 1.f), 0.f, 1.f);
            if (light.confidence < .75f) { pos = end + 1; continue; }
            const uint64_t x = uint64_t(std::llround(light.position.x * 16.f));
            const uint64_t y = uint64_t(std::llround(light.position.y * 16.f));
            const uint64_t z = uint64_t(std::llround(light.position.z * 16.f));
            light.stableId = 0x4d414e4900000000ull ^ (x * 0x9e3779b185ebca87ull) ^ (y << 21) ^ (z << 42);
            light.flags = LocalLightPoint | LocalLightManifest | LocalLightVolumetric;
            m_manifestLights.push_back(light);
            pos = end + 1;
        }
        Log("Loaded authoritative world-space manifest lights: " + std::to_string(m_manifestLights.size()));
    }

    void LocalLightManager::OnSetLight(DWORD index, const D3DLIGHT9* light)
    {
        if (!light) return;
        NativeSlot& slot = m_nativeLights[index];
        bool stable = slot.valid;
        if (slot.valid)
        {
            const float dx = slot.light.Position.x - light->Position.x;
            const float dy = slot.light.Position.y - light->Position.y;
            const float dz = slot.light.Position.z - light->Position.z;
            // WoW frequently reuses a fixed-function light slot for short-lived
            // spell/particle draws.  A position jump starts a new observation;
            // only persistent world-space sources are admitted below.
            if (dx * dx + dy * dy + dz * dz > 0.25f)
            {
                slot.stableFrames = 0;
                ++slot.generation;
                stable = false;
            }
        }
        slot.light = *light;
        slot.valid = light->Type == D3DLIGHT_POINT || light->Type == D3DLIGHT_SPOT;
        slot.lastSeenTick = GetTickCount64();
        if (slot.valid && stable)
            slot.stableFrames = std::min<uint32_t>(slot.stableFrames + 1, 100000u);
    }

    void LocalLightManager::OnLightEnable(DWORD index, BOOL enabled)
    {
        NativeSlot& slot = m_nativeLights[index];
        slot.enabled = enabled != FALSE;
        if (slot.enabled)
        {
            slot.lastEnabledTick = GetTickCount64();
            slot.stableFrames = std::min<uint32_t>(slot.stableFrames + 1, 100000u);
        }
    }

    float LocalLightManager::EstimateRadius(const D3DLIGHT9& light)
    {
        if (std::isfinite(light.Range) && light.Range > 0.25f && light.Range < 500.0f)
            return light.Range;
        const float q = std::max(light.Attenuation2, 0.0001f);
        const float l = std::max(light.Attenuation1, 0.0f);
        const float c = std::max(light.Attenuation0, 0.01f) - 32.0f;
        const float disc = l * l - 4.0f * q * c;
        return std::clamp(disc > 0.0f ? (-l + std::sqrt(disc)) / (2.0f * q) : 12.0f, 2.0f, 80.0f);
    }

    void LocalLightManager::ObserveTorchDraw(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT start,UINT primitives,bool indexed,const void* verticesUP,UINT strideUP,const void* indicesUP,D3DFORMAT indexFormatUP,UINT vertexCountUP)
    {
        // Only verified flame material + verified rigid M2 skinning. Neither
        // arbitrary bright pixels nor character/weapon textures become lights.
        const auto& f=FrameContext::Current();
        if(!m_enabled||!d||!f.cameraValid||!g_trackedState.zEnable||!g_trackedState.alphaBlend)return;
        bool rigid=std::find(std::begin(kRigidM2Shaders),std::end(kRigidM2Shaders),g_trackedState.vsHash)!=std::end(kRigidM2Shaders);
        bool unskinned=std::find(std::begin(kUnskinnedM2Shaders),std::end(kUnskinnedM2Shaders),g_trackedState.vsHash)!=std::end(kUnskinnedM2Shaders);
        if(!rigid&&!unskinned)return;
        using Microsoft::WRL::ComPtr;
        ComPtr<IDirect3DBaseTexture9> texture;if(FAILED(d->GetTexture(0,texture.GetAddressOf())))return;
        uint64_t hash=TextureHashLookup::Instance().GetTextureHash(texture.Get());
        if(std::find(std::begin(kTorchTextureHashes),std::end(kTorchTextureHashes),hash)==std::end(kTorchTextureHashes))return;
        UINT count=type==D3DPT_TRIANGLELIST?primitives*3:type==D3DPT_TRIANGLESTRIP?primitives+2:0;
        if(count<3||count>768)return;
        ComPtr<IDirect3DVertexDeclaration9> decl;D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1];UINT ne=std::size(elements);
        if(FAILED(d->GetVertexDeclaration(decl.GetAddressOf()))||!decl||FAILED(decl->GetDeclaration(elements,&ne)))return;
        int position=-1,bone=-1;
        for(UINT i=0;i<ne;++i){const auto& e=elements[i];if(e.Stream!=0)continue;
            if(e.Usage==D3DDECLUSAGE_POSITION&&e.Type==D3DDECLTYPE_FLOAT3)position=e.Offset;
            if(e.Usage==D3DDECLUSAGE_BLENDINDICES&&e.Type==D3DDECLTYPE_UBYTE4)bone=e.Offset;}
        if(position<0||(rigid&&bone<0))return;
        ComPtr<IDirect3DIndexBuffer9> ib;ComPtr<IDirect3DVertexBuffer9> vb;UINT offset=0,stride=strideUP;
        D3DVERTEXBUFFER_DESC vd{};
        if(!verticesUP){if(FAILED(d->GetStreamSource(0,vb.GetAddressOf(),&offset,&stride))||!vb||FAILED(vb->GetDesc(&vd)))return;}
        if(stride<UINT(position+12)||(rigid&&stride<UINT(bone+4)))return;
        const void* indices=indicesUP;D3DFORMAT indexFormat=indexFormatUP;void* lockedIndices=nullptr;
        if(indexed&&!indicesUP){D3DINDEXBUFFER_DESC id{};
            if(FAILED(d->GetIndices(ib.GetAddressOf()))||!ib||FAILED(ib->GetDesc(&id)))return;
            indexFormat=id.Format;UINT size=indexFormat==D3DFMT_INDEX16?2:4;
            if((uint64_t(start)+count)*size>id.Size)return;
            if(FAILED(ib->Lock(start*size,count*size,&lockedIndices,D3DLOCK_READONLY)))return;indices=lockedIndices;}
        std::vector<UINT> selected;UINT lo=~0u,hi=0;bool valid=true;
        for(UINT i=0;i<count;++i){int64_t v=int64_t(base)+(indexed?(indexFormat==D3DFMT_INDEX16?static_cast<const WORD*>(indices)[i]:static_cast<const DWORD*>(indices)[i]):start+i);
            if(v<0||(!verticesUP&&uint64_t(offset)+(uint64_t(v)+1)*stride>vd.Size)||(verticesUP&&vertexCountUP&&uint64_t(v)>=vertexCountUP)){valid=false;break;}
            selected.push_back(UINT(v));lo=std::min(lo,UINT(v));hi=std::max(hi,UINT(v));}
        if(lockedIndices)ib->Unlock();if(!valid||selected.empty()||uint64_t(hi-lo+1)*stride>1024*1024)return;
        void* lockedVertices=nullptr;const BYTE* vertices=static_cast<const BYTE*>(verticesUP);
        if(!verticesUP){if(FAILED(vb->Lock(offset+lo*stride,(hi-lo+1)*stride,&lockedVertices,D3DLOCK_READONLY))){static bool logged=false;if(!logged){Log("Torch draw matched but vertex buffer is not readable");logged=true;}return;}
            vertices=static_cast<const BYTE*>(lockedVertices);}
        float matrices[225][4]{};HRESULT constants=d->GetVertexShaderConstantF(31,matrices[0],rigid?225:3);
        std::vector<Vec3> centers;std::vector<UINT> centerCounts;
        if(SUCCEEDED(constants))for(UINT index:selected){const BYTE* vertex=vertices+(index-(verticesUP?0:lo))*stride;
            UINT bi=rigid?vertex[bone]:0;if(bi>=75)continue;float v[3];memcpy(v,vertex+position,sizeof(v));
            Vec3 p;float* components=&p.x;for(UINT j=0;j<3;++j){const float* row=matrices[bi*3+j];components[j]=row[0]*v[0]+row[1]*v[1]+row[2]*v[2]+row[3];}
            if(!std::isfinite(Length(p)))continue;
            size_t cluster=0;while(cluster<centers.size()&&Length(centers[cluster]-p)>1.5f)++cluster;
            if(cluster==centers.size()){if(centers.size()>=32)continue;centers.push_back(p);centerCounts.push_back(1);}
            else {UINT n=++centerCounts[cluster];centers[cluster]=centers[cluster]+(p-centers[cluster])*(1.f/n);}}
        if(lockedVertices)vb->Unlock();
        // Particle batches can contain several torches. Never average the
        // whole batch into a phantom emitter halfway between real flames.
        for(const Vec3& view:centers){
        Vec3 world=f.cameraPosition+Vec3{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2]}*view.x+
            Vec3{f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2]}*view.y+
            Vec3{f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2]}*view.z;
        if(!std::isfinite(Length(world))||Length(world-f.cameraPosition)>120)continue;
        ULONGLONG now=GetTickCount64();
        auto found=std::find_if(m_attached.begin(),m_attached.end(),[&](const AttachedEmitter& e){return now-e.seen<kAttachedLifetimeMs&&Length(e.light.position-world)<1.5f;});
        if(found!=m_attached.end()){found->light.position=world;found->seen=now;continue;}
        if(m_attached.size()>=64)return;
        LocalLightSource light;light.stableId=0x544f524300000000ull|m_nextAttachedId++;light.position=world;
        light.color={1.f,.52f,.16f};light.radius=10;light.intensity=1.8f;
        light.flags=LocalLightPoint|LocalLightAttached|LocalLightVolumetric;m_attached.push_back({light,now});
        Log("Captured attached flame emitter id="+std::to_string(light.stableId));
        }
    }

    void LocalLightManager::SelectForFrame(const Vec3& cameraPosition, const Vec3& cameraForward)
    {
        ++m_selectionFrame;
        m_selected.clear();
        if (!m_enabled || m_intensityScale <= 0.0f) return;

        std::vector<LocalLightSource> candidates;
        candidates.reserve(m_nativeLights.size() + m_manifestLights.size());
        for (auto& pair : m_nativeLights)
        {
            const DWORD index = pair.first;
            NativeSlot& slot = pair.second;
            // WoW enables many model/world lights only for the draw calls
            // which consume them and disables the slot again before our
            // end-of-world composite.  Treat a recently enabled, repeatedly
            // observed slot as alive for a short grace window instead of
            // sampling only the final (usually disabled) state.
            const ULONGLONG now = GetTickCount64();
            const bool recentlyEnabled = slot.enabled ||
                (slot.lastEnabledTick != 0 && now - slot.lastEnabledTick <= 180);
            const bool recentlySeen = slot.lastSeenTick != 0 && now - slot.lastSeenTick <= 350;
            if (!slot.valid || !recentlyEnabled || !recentlySeen || slot.stableFrames < 6) continue;
            const D3DLIGHT9& d = slot.light;
            LocalLightSource light;
            light.stableId = 0x4e41544900000000ull ^ uint64_t(index) ^ (uint64_t(slot.generation) << 24);
            light.position = { d.Position.x, d.Position.y, d.Position.z };
            light.direction = Normalize({ d.Direction.x, d.Direction.y, d.Direction.z });
            light.color = {
                std::max(0.0f, d.Diffuse.r),
                std::max(0.0f, d.Diffuse.g),
                std::max(0.0f, d.Diffuse.b)
            };
            light.radius = EstimateRadius(d);
            light.intensity = std::clamp(std::max({ light.color.x, light.color.y, light.color.z }), 0.05f, 8.0f);
            if (light.intensity > 0.001f)
            {
                light.color = light.color * (1.0f / light.intensity);
            }
            light.innerCone = d.Type == D3DLIGHT_SPOT ? std::cos(d.Theta * 0.5f) : -1.0f;
            light.outerCone = d.Type == D3DLIGHT_SPOT ? std::cos(d.Phi * 0.5f) : -1.0f;
            light.flags = (d.Type == D3DLIGHT_SPOT ? LocalLightSpot : LocalLightPoint) |
                          LocalLightNative | LocalLightVolumetric;
            light.confidence = 1.0f;

            const Vec3 delta = light.position - cameraPosition;
            const float distance = Length(delta);
            if (!std::isfinite(distance) || distance > light.radius * 2.0f + 80.0f) continue;
            const float forwardDistance = Dot(delta, cameraForward);
            if (forwardDistance < -light.radius) continue;
            const float facing = std::clamp(forwardDistance / std::max(distance, 0.01f) * 0.35f + 0.65f, 0.15f, 1.0f);
            light.score = light.intensity * light.radius * light.radius * facing / std::max(4.0f, distance * distance);

            auto previous = m_smoothed.find(light.stableId);
            if (previous != m_smoothed.end())
            {
                constexpr float k = 0.24f;
                light.position = previous->second.position * (1.0f - k) + light.position * k;
                light.color = previous->second.color * (1.0f - k) + light.color * k;
                light.intensity = previous->second.intensity * (1.0f - k) + light.intensity * k;
            }
            m_smoothed[light.stableId] = light;
            candidates.push_back(light);
        }

        const ULONGLONG emitterNow=GetTickCount64();
        std::erase_if(m_attached,[&](const AttachedEmitter& e){return emitterNow-e.seen>kAttachedLifetimeMs;});
        for(auto& emitter:m_attached){auto light=emitter.light;float distance=Length(light.position-cameraPosition);
            if(distance>100)continue;
            const float age=float(emitterNow-emitter.seen);
            light.intensity*=age<=kAttachedHoldMs?1.f:std::clamp(1.f-(age-kAttachedHoldMs)/float(kAttachedLifetimeMs-kAttachedHoldMs),0.f,1.f);
            light.score=4.f*light.intensity*light.radius*light.radius/std::max(4.f,distance*distance);candidates.push_back(light);}

        for (auto light : m_manifestLights)
        {
            const Vec3 delta = light.position - cameraPosition;
            const float distance = Length(delta);
            // Camera basis conventions differ between the fixed-function and
            // shader paths in this client.  Distance is authoritative; the
            // surface shader itself naturally rejects lights that cannot
            // affect visible pixels.  A forward-dot gate was discarding even
            // lamps visibly in front of the player in Goldshire.
            if (!std::isfinite(distance) || distance > light.radius * 2.f + 80.f) continue;
            light.score = light.intensity * light.radius * light.radius / std::max(4.f, distance * distance);
            candidates.push_back(light);
        }

        std::sort(candidates.begin(), candidates.end(), [](const LocalLightSource& a, const LocalLightSource& b) {
            return a.score > b.score;
        });
        const size_t limit = m_quality == 0 ? 4u : 8u;
        if (candidates.size() > limit) candidates.resize(limit);
        std::vector<LocalLightSource> actorLights;
        ActorLightManager::Instance().Append(actorLights);
        std::erase_if(candidates, [](const LocalLightSource& l){return ActorLightManager::Instance().SuppressNative(l);});
        // Native lights retain their old budget. Actor bank adds 1 player + <=23 creatures.
        candidates.insert(candidates.end(),actorLights.begin(),actorLights.end());
        if(candidates.size()>32)candidates.resize(32);
        m_selected = std::move(candidates);

        uint64_t signature = 1469598103934665603ull;
        for (const auto& light : m_selected)
        {
            signature ^= light.stableId;
            signature *= 1099511628211ull;
        }
        m_selectionSignature = signature;

        if ((m_selectionFrame % 300) == 0)
        {
            float nearest = 1e30f;
            for (const auto& light : m_manifestLights)
                nearest = std::min(nearest, Length(light.position - cameraPosition));
            size_t nativeSelected = 0;
            for (const auto& light : m_selected)
                if (light.flags & LocalLightNative) ++nativeSelected;
            Log("Source diagnostic: native_slots=" + std::to_string(m_nativeLights.size()) +
                " selected=" + std::to_string(m_selected.size()) +
                " native_selected=" + std::to_string(nativeSelected) + " attached=" + std::to_string(m_attached.size()) +
                " camera=(" + std::to_string(cameraPosition.x) + "," +
                std::to_string(cameraPosition.y) + "," + std::to_string(cameraPosition.z) +
                ") nearest_manifest=" + std::to_string(nearest));
        }
    }

    void LocalLightManager::Log(const std::string& message) const
    {
        if (m_logPath.empty()) return;
        std::ofstream out(std::filesystem::path(m_logPath), std::ios::app);
        out << message << '\n';
    }
}
