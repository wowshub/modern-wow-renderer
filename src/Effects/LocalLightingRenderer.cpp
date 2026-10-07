#include "LocalLightingRenderer.h"

#include <algorithm>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include "../D3D9/DepthCapture.h"
#include "../D3D9/ScopedRenderState.h"
#include "../Lighting/LocalLightManager.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
namespace
{
const char* kSurfaceSource = R"HLSL(
sampler2D depthMap:register(s0);
sampler2D waterMask:register(s1);
float4 cameraPos:register(c0);
float4 projection:register(c1); // x=A,y=B,z=P00,w=P11
float4 inv0:register(c2); float4 inv1:register(c3); float4 inv2:register(c4);
float4 texel:register(c5);       // xy=full-res texel, z=count, w=occluded count
float4 tuning:register(c6);      // x=intensity, y=debug
float4 lightPosRadius[32]:register(c8);
float4 lightColorPower[32]:register(c40);
float4 lightDirectionCone[32]:register(c72);

float Linear(float raw) { return projection.y/(raw-projection.x); }
float3 World(float2 uv,float raw) {
 float z=Linear(raw);
 float3 v=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;
 return cameraPos.xyz+v.x*inv0.xyz+v.y*inv1.xyz+v.z*inv2.xyz;
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float raw=tex2D(depthMap,uv).r;
 if(raw>=.99990||tex2D(waterMask,uv).r>.25) return float4(0,0,0,0);
 float3 p=World(uv,raw);
 float dl=tex2D(depthMap,uv-float2(texel.x,0)).r,dr=tex2D(depthMap,uv+float2(texel.x,0)).r;
 float du=tex2D(depthMap,uv-float2(0,texel.y)).r,dd=tex2D(depthMap,uv+float2(0,texel.y)).r;
 float3 px=abs(Linear(dl)-Linear(raw))<abs(Linear(dr)-Linear(raw))?World(uv-float2(texel.x,0),dl):World(uv+float2(texel.x,0),dr);
 float3 py=abs(Linear(du)-Linear(raw))<abs(Linear(dd)-Linear(raw))?World(uv-float2(0,texel.y),du):World(uv+float2(0,texel.y),dd);
 float3 n=normalize(cross(px-p,py-p));
 float3 toEye=normalize(cameraPos.xyz-p); if(dot(n,toEye)<0)n=-n;
 float3 creatureSum=0; float3 sum=0,sourceDebug=0; float visibilityDebug=0; int count=(int)texel.z; int occ=(int)texel.w;
 [loop] for(int i=0;i<32;++i) {
  if(i>=count) break;
  float3 toL=lightPosRadius[i].xyz-p; float dist=length(toL); float radius=lightPosRadius[i].w;
  float3 l=toL/max(dist,.001); float edge=saturate(1-dist/max(radius,.01));
  float attenuation=edge*edge/(1+dist*dist*.035);
  float cone=lightDirectionCone[i].w;
  if(cone>-0.5) attenuation*=smoothstep(cone,min(1.0,cone+.12),dot(-l,normalize(lightDirectionCone[i].xyz)));
  float diffuse=saturate(dot(n,l));
  float spec=pow(saturate(dot(reflect(-l,n),toEye)),24)*.055;
  float visibility=1; // Preserve native shadows; no duplicate screen-depth shadows.
  float3 contribution=lightColorPower[i].rgb*lightColorPower[i].w*(diffuse+spec)*attenuation*visibility;
  if(cone < -1.5) creatureSum+=contribution*saturate(n.z*2); else sum+=contribution;
  sourceDebug+=lightColorPower[i].rgb*lightColorPower[i].w*edge*edge;
  visibilityDebug+=attenuation*(1-visibility);
 }
 sum+=min(creatureSum,0.12);
 sum*=tuning.x;
 if(tuning.y>.5&&tuning.y<1.5) return float4(sum,Linear(raw));
 if(tuning.y>1.5&&tuning.y<2.5) return float4(sum,Linear(raw));
 if(tuning.y>2.5&&tuning.y<3.5) return float4(saturate(visibilityDebug).xxx,Linear(raw));
 return float4(min(sum,3.0),Linear(raw));
})HLSL";

const char* kCompositeSource = R"HLSL(
sampler2D sceneMap:register(s0); sampler2D lightMap:register(s1);
sampler2D depthMap:register(s2); sampler2D waterMask:register(s3);
float4 texel:register(c0); // xy=light texel, zw unused
float4 tuning:register(c1); // x=debug mode
float4 projection:register(c2);
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float4 scene=tex2D(sceneMap,uv); float raw=tex2D(depthMap,uv).r;
 if(raw>=.99990||tex2D(waterMask,uv).r>.25)return scene;
 float receiverZ=projection.y/(raw-projection.x);
 // Account for a continuous depth slope across a low-resolution texel.
 // Use the smaller one-sided differences so silhouettes do not inflate it.
 float2 fullTexel=texel.zw;
 float zl=projection.y/(tex2D(depthMap,uv-float2(fullTexel.x,0)).r-projection.x);
 float zr=projection.y/(tex2D(depthMap,uv+float2(fullTexel.x,0)).r-projection.x);
 float zu=projection.y/(tex2D(depthMap,uv-float2(0,fullTexel.y)).r-projection.x);
 float zd=projection.y/(tex2D(depthMap,uv+float2(0,fullTexel.y)).r-projection.x);
 float slope=min(abs(zl-receiverZ),abs(zr-receiverZ))+min(abs(zu-receiverZ),abs(zd-receiverZ));
 float tolerance=max(max(.12,receiverZ*.008),slope*max(texel.x/fullTexel.x,texel.y/fullTexel.y)*1.5);
 float2 size=1/texel.xy; float2 pixel=uv*size-.5; float2 base=floor(pixel); float2 f=frac(pixel);
 float2 o[4]={float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
 float3 light=0; float sum=0; float best=1e9; float3 nearest=0;
 [unroll]for(int i=0;i<4;++i){
  float2 q=(base+o[i]+.5)*texel.xy; float4 s=tex2D(lightMap,q);
  float d=abs(s.a-receiverZ); float2 aw=1-abs(o[i]-f); float w=max(aw.x*aw.y,.001)*exp(-d/max(tolerance,.01)*3);
  light+=s.rgb*w;sum+=w;if(d<best){best=d;nearest=s.rgb;}
 }
 light=sum>.02?light/sum:(best<tolerance?nearest:float3(0,0,0));
 if(tuning.x>.5)return float4(light,1);
 return float4(scene.rgb+light,scene.a);
})HLSL";

bool Compile(IDirect3DDevice9* device, const char* source, IDirect3DPixelShader9** shader, std::string& error)
{
    ComPtr<ID3DBlob> blob, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "ps_3_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) || !blob)
    {
        if (errors) error.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        return false;
    }
    if (FAILED(device->CreatePixelShader(static_cast<const DWORD*>(blob->GetBufferPointer()), shader)))
    {
        error = "CreatePixelShader failed";
        return false;
    }
    return true;
}
}

LocalLightingRenderer& LocalLightingRenderer::Instance()
{
    static LocalLightingRenderer renderer;
    return renderer;
}

void LocalLightingRenderer::Configure(const std::wstring& basePath)
{
    m_logPath = basePath + L"LocalLighting.log";
    m_sessionDisabled = false;
}

void LocalLightingRenderer::Log(const std::string& message) const
{
    if (m_logPath.empty()) return;
    std::ofstream(std::filesystem::path(m_logPath), std::ios::app) << message << '\n';
}

void LocalLightingRenderer::Reset(IDirect3DDevice9* device)
{
    if (m_owner && device && m_owner != device) return;
    m_lightSurface.Reset(); m_lightTexture.Reset();
    m_litSurface.Reset(); m_litTexture.Reset();
    m_surfaceShader.Reset(); m_compositeShader.Reset();
    m_owner = nullptr; m_width = m_height = m_lightWidth = m_lightHeight = 0;
    m_format = D3DFMT_UNKNOWN; m_quality = -1; m_sessionDisabled = false;
}

bool LocalLightingRenderer::EnsureShaders(IDirect3DDevice9* device)
{
    if (m_surfaceShader && m_compositeShader) return true;
    std::string error;
    if (!Compile(device, kSurfaceSource, m_surfaceShader.GetAddressOf(), error) ||
        !Compile(device, kCompositeSource, m_compositeShader.GetAddressOf(), error))
    {
        Log("Dynamic lighting disabled for this device: " + error);
        m_surfaceShader.Reset(); m_compositeShader.Reset(); m_sessionDisabled = true;
        return false;
    }
    return true;
}

bool LocalLightingRenderer::EnsureResources(IDirect3DDevice9* device, uint32_t width, uint32_t height, D3DFORMAT format, int quality)
{
    const float scale = quality <= 0 ? 0.5f : (quality == 1 ? 0.75f : 1.f);
    const uint32_t lw = std::max(1u, uint32_t(width * scale));
    const uint32_t lh = std::max(1u, uint32_t(height * scale));
    if (m_owner == device && m_width == width && m_height == height && m_lightWidth == lw &&
        m_lightHeight == lh && m_format == format && m_quality == quality && m_lightTexture && m_litTexture) return true;

    m_lightSurface.Reset(); m_lightTexture.Reset(); m_litSurface.Reset(); m_litTexture.Reset();
    m_owner = device; m_width = width; m_height = height; m_lightWidth = lw; m_lightHeight = lh;
    m_format = format; m_quality = quality;
    D3DFORMAT lightFormat = D3DFMT_A16B16G16R16F;
    // Alpha now stores linear receiver depth, which cannot be represented in
    // an 8-bit normalized fallback. Fail this optional pass instead of leaking
    // another surface's lighting across silhouettes on unsupported hardware.
    if (FAILED(device->CreateTexture(lw, lh, 1, D3DUSAGE_RENDERTARGET, lightFormat, D3DPOOL_DEFAULT, m_lightTexture.GetAddressOf(), nullptr))) return false;
    if (FAILED(m_lightTexture->GetSurfaceLevel(0, m_lightSurface.GetAddressOf())) ||
        FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, m_litTexture.GetAddressOf(), nullptr)) ||
        FAILED(m_litTexture->GetSurfaceLevel(0, m_litSurface.GetAddressOf()))) return false;
    return true;
}

void LocalLightingRenderer::DrawQuad(IDirect3DDevice9* device, uint32_t width, uint32_t height)
{
    struct V { float x, y, z, rhw, u, v; };
    const float w = float(width) - .5f, h = float(height) - .5f;
    V q[] = { {-.5f,-.5f,0,1,0,0},{w,-.5f,0,1,1,0},{-.5f,h,0,1,0,1},{w,h,0,1,1,1} };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

bool LocalLightingRenderer::Render(IDirect3DDevice9* device, FrameContext& f)
{
    auto& manager = LocalLightManager::Instance();
    manager.SelectForFrame(f.cameraPosition, { f.inverseView.m[2][0], f.inverseView.m[2][1], f.inverseView.m[2][2] });
    const auto& lights = manager.Selected();
    if (m_sessionDisabled || !manager.Enabled() || lights.empty() || !device || !f.cameraValid || !f.depthAvailable || !f.sceneColor) return false;
    D3DSURFACE_DESC desc{};
    if (!f.sceneSurface || FAILED(f.sceneSurface->GetDesc(&desc)) || !EnsureShaders(device) ||
        !EnsureResources(device, desc.Width, desc.Height, desc.Format, manager.Quality()))
    {
        if (!m_sessionDisabled) Log("Dynamic lighting render targets unavailable; feature skipped without affecting the scene");
        m_sessionDisabled = true;
        return false;
    }

    ScopedRenderState state(device);
    DepthCapture::Instance().RawSetDepth(device, nullptr);
    device->SetVertexShader(nullptr);
    for (auto rs : { D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE,D3DRS_ALPHABLENDENABLE })
        device->SetRenderState(rs, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    for (DWORD s = 0; s < 4; ++s)
    {
        device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    D3DVIEWPORT9 low{ 0,0,m_lightWidth,m_lightHeight,0,1 };
    device->SetViewport(&low); device->SetRenderTarget(0, m_lightSurface.Get());
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
    device->SetPixelShader(m_surfaceShader.Get()); device->SetTexture(0, f.depthTexture); device->SetTexture(1, f.waterMaskTexture);
    const float camera[4] = { f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,0 };
    const float inv[3][4] = { {f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},{f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},{f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0} };
    const int maxLights = 32;
    const int count = std::min<int>(maxLights, int(lights.size()));
    const int occluded = std::min(count, manager.Quality() <= 0 ? 2 : 4);
    const float texel[4] = { 1.f / desc.Width,1.f / desc.Height,float(count),float(occluded) };
    const float tuning[4] = { manager.IntensityScale()*f.environment[Lighting],float(manager.DebugMode()),0,0 };
    float pos[32][4]{}, color[32][4]{}, direction[32][4]{};
    for (int i = 0; i < count; ++i)
    {
        pos[i][0] = lights[i].position.x; pos[i][1] = lights[i].position.y; pos[i][2] = lights[i].position.z; pos[i][3] = lights[i].radius;
        color[i][0] = lights[i].color.x; color[i][1] = lights[i].color.y; color[i][2] = lights[i].color.z; color[i][3] = lights[i].intensity;
        direction[i][0] = lights[i].direction.x; direction[i][1] = lights[i].direction.y; direction[i][2] = lights[i].direction.z;
        direction[i][3] = (lights[i].flags & LocalLightCreature) ? -2.f : (lights[i].flags & LocalLightSpot) ? lights[i].outerCone : -1.f;
    }
    // This pass samples raw INTZ, unlike atmosphere's normalized boundary.
    // Fold the viewport transform into A/B so both World and Visibility use
    // exactly the depth convention that WoW wrote (including MaxZ < 1).
    const float depthRange = std::max(f.depthMaxZ - f.viewport.MinZ, .001f);
    const float projection[4] = { f.viewport.MinZ + f.projUnpack[0] * depthRange,
        f.projUnpack[1] * depthRange, f.projUnpack[2], f.projUnpack[3] };
    device->SetPixelShaderConstantF(0, camera, 1); device->SetPixelShaderConstantF(1, projection, 1);
    device->SetPixelShaderConstantF(2, inv[0], 3); device->SetPixelShaderConstantF(5, texel, 1); device->SetPixelShaderConstantF(6, tuning, 1);
    device->SetPixelShaderConstantF(8, pos[0], 32); device->SetPixelShaderConstantF(40, color[0], 32); device->SetPixelShaderConstantF(72, direction[0], 32);
    DrawQuad(device, m_lightWidth, m_lightHeight);
    device->SetTexture(0, nullptr); device->SetTexture(1, nullptr);

    D3DVIEWPORT9 full{ 0,0,desc.Width,desc.Height,0,1 };
    device->SetViewport(&full); device->SetRenderTarget(0, m_litSurface.Get()); device->SetPixelShader(m_compositeShader.Get());
    device->SetTexture(0, f.sceneColor); device->SetTexture(1, m_lightTexture.Get()); device->SetTexture(2, f.depthTexture); device->SetTexture(3, f.waterMaskTexture);
    const float lightTexel[4] = { 1.f / m_lightWidth,1.f / m_lightHeight,1.f / m_width,1.f / m_height };
    const float debug[4] = { (manager.DebugMode() >= 1 && manager.DebugMode() <= 3) ? 1.f : 0.f,0,0,0 };
    device->SetPixelShaderConstantF(0, lightTexel, 1); device->SetPixelShaderConstantF(1, debug, 1);
    device->SetPixelShaderConstantF(2, projection, 1);
    DrawQuad(device, desc.Width, desc.Height);
    for (DWORD s = 0; s < 4; ++s) device->SetTexture(s, nullptr);
    f.sceneColor = m_litTexture.Get(); f.sceneSurface = m_litSurface.Get();
    return true;
}
}
