#pragma once

#include <d3d9.h>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "../Core/MathTypes.h"

namespace renderer
{
    enum LocalLightFlags : uint32_t
    {
        LocalLightPoint = 1u << 0,
        LocalLightSpot = 1u << 1,
        LocalLightNative = 1u << 2,
        LocalLightManifest = 1u << 3,
        LocalLightVolumetric = 1u << 4,
        LocalLightAttached = 1u << 5,
        LocalLightPlayer = 1u << 6,
        LocalLightCreature = 1u << 7
    };

    struct LocalLightSource
    {
        uint64_t stableId = 0;
        Vec3 position{};
        Vec3 direction{ 0.0f, 0.0f, -1.0f };
        Vec3 color{ 1.0f, 0.78f, 0.48f };
        float radius = 12.0f;
        float intensity = 1.0f;
        float innerCone = 1.0f;
        float outerCone = -1.0f;
        uint32_t flags = LocalLightPoint | LocalLightVolumetric;
        float confidence = 1.0f;
        float score = 0.0f;
    };

    class LocalLightManager
    {
    public:
        static LocalLightManager& Instance();

        void Configure(const std::wstring& basePath);
        void ReloadTuning();
        void Reset();

        void OnSetLight(DWORD index, const D3DLIGHT9* light);
        void OnLightEnable(DWORD index, BOOL enabled);
        void ObserveTorchDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT start, UINT primitives, bool indexed = true, const void* verticesUP = nullptr, UINT strideUP = 0, const void* indicesUP = nullptr, D3DFORMAT indexFormatUP = D3DFMT_INDEX16, UINT vertexCountUP = 0);
        void SelectForFrame(const Vec3& cameraPosition, const Vec3& cameraForward);

        bool Enabled() const { return m_enabled; }
        float IntensityScale() const { return m_intensityScale; }
        float RayScale() const { return m_rayScale; }
        int Quality() const { return m_quality; }
        int DebugMode() const { return m_debugMode; }
        const std::vector<LocalLightSource>& Selected() const { return m_selected; }
        uint64_t SelectionSignature() const { return m_selectionSignature; }

    private:
        struct NativeSlot
        {
            D3DLIGHT9 light{};
            bool valid = false;
            bool enabled = false;
            uint32_t stableFrames = 0;
            uint32_t generation = 0;
            ULONGLONG lastSeenTick = 0;
            ULONGLONG lastEnabledTick = 0;
        };

        LocalLightManager() = default;
        void Log(const std::string& message) const;
        void LoadManifest();
        static float EstimateRadius(const D3DLIGHT9& light);

        std::wstring m_basePath;
        std::wstring m_iniPath;
        std::wstring m_logPath;
        std::unordered_map<DWORD, NativeSlot> m_nativeLights;
        std::unordered_map<uint64_t, LocalLightSource> m_smoothed;
        std::vector<LocalLightSource> m_manifestLights;
        struct AttachedEmitter { LocalLightSource light; ULONGLONG seen; };
        std::vector<AttachedEmitter> m_attached;
        uint64_t m_nextAttachedId = 1;
        std::vector<LocalLightSource> m_selected;
        bool m_enabled = true;
        float m_intensityScale = 1.0f;
        float m_rayScale = 0.75f;
        int m_quality = 1;
        int m_debugMode = 0;
        uint64_t m_selectionSignature = 0;
        uint64_t m_selectionFrame = 0;
    };
}
