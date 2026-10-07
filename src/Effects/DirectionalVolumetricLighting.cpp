#include "../Environment/LocationTuning.h"
#include "DirectionalVolumetricLighting.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "../D3D9/ScopedRenderState.h"
#include "../Diagnostics/PerformanceProfiler.h"
#include "EnvironmentFogCapture.h"
#include "GroundSurfaceCapture.h"
#include "../Lighting/LocalLightManager.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace renderer
{
namespace
{
// Atmosphere boundary depth: scene depth everywhere, except at water
// pixels, where it's replaced with the water SURFACE's own depth. Water
// never writes the main depth buffer (WaterEffect samples "what's behind
// water" through it, which only works if water leaves it alone), so
// without this, every downstream pass - the low-res downsample, the
// bilateral upsample, temporal reprojection - would see straight through
// to the seabed and integrate air fog all the way down to it, producing a
// cyan/bright wash over underwater terrain that has nothing to do with
// air. watereffect::Scope writes water's own view-space depth (viewPos.z)
// into a second render target during the real water draws; re-encode it
// into the same raw hyperbolic depth convention as the real depth buffer
// (raw = A + B/z, the inverse of the z = B/(raw-A) reconstruction used
// everywhere else) so every downstream shader can keep treating this
// exactly like scene depth with no further changes.
const char* kBoundarySource = R"HLSL(
sampler2D sceneDepth:register(s0); sampler2D waterCoverage:register(s1); sampler2D waterDepth:register(s2);
float4 projection:register(c0); // x=A, y=B, z=viewport MinZ, w=viewport depth range
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float raw=tex2D(sceneDepth,uv).r;
 // INTZ stores viewport depth. Normalize once, before all atmosphere passes.
 // Otherwise MaxZ=.94 turns even a 100-unit surface into roughly 1.6 units.
 raw=raw>=.99999?1:saturate((raw-projection.z)/max(projection.w,.001));
 float coverage=tex2D(waterCoverage,uv).r;
 if(coverage>.35) {
   float wz=tex2D(waterDepth,uv).r;
   if(wz>.05) raw=saturate(projection.x+projection.y/wz);
 }
 return raw.xxxx;
})HLSL";

// Debug-only: visualize kBoundarySource's output directly as linear
// distance, normalized by max fog distance. Over water this must read as
// the SURFACE distance, not the seabed - a quick way to confirm the water
// depth MRT write is actually reaching the atmosphere pass.
const char* kBoundaryDebugSource = R"HLSL(
sampler2D boundaryDepth:register(s0);
float4 projection:register(c0); // x=A,y=B,z=maxDistance
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float raw=tex2D(boundaryDepth,uv).r;
 float z=raw>=.9999?projection.z:projection.y/(raw-projection.x);
 return saturate(z/max(projection.z,1.0)).xxxx;
})HLSL";

// Two pixels of GPU-only persistent state: player/ground and planar velocity.
// No readback or synchronization with the game render thread.
const char* kGroundHeightSource = R"HLSL(
sampler2D boundaryDepth:register(s0); sampler2D previousState:register(s1);
float4 cameraPos:register(c0); float4 projection:register(c1);
float4 invView0:register(c2); float4 invView1:register(c3); float4 invView2:register(c4);
float4 timing:register(c5); // dt, history valid, ground tracking, unused
float3 World(float2 uv) {
 float raw=tex2Dlod(boundaryDepth,float4(uv,0,0)).r;
 float z=projection.y/(raw-projection.x);
 if(raw>=.9999 || z<0 || z>100) return cameraPos.xyz+float3(0,0,-5);
 float3 v=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;
 return cameraPos.xyz+v.x*invView0.xyz+v.y*invView1.xyz+v.z*invView2.xyz;
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float a=World(float2(.25,.90)).z,b=World(float2(.50,.90)).z,c=World(float2(.75,.90)).z;
 float ground=a+b+c-min(a,min(b,c))-max(a,max(b,c));
 // Third-person silhouette probe, rather than the camera's world position.
 float3 actor=World(float2(.5,.60));
 float best=1e6;bool detected=false;
 [unroll]for(int i=0;i<4;++i){float3 candidate=World(float2(.5,.44+i*.055));
  float height=candidate.z-ground,dist=length(candidate-cameraPos.xyz);
  if(height>.6&&height<5.5&&dist<best){actor=candidate;best=dist;detected=true;}}

 float4 old=tex2Dlod(previousState,float4(.25,.5,0,0));
 float4 velocity=tex2Dlod(previousState,float4(.75,.5,0,0));
 if(timing.y<.5) return uv.x<.5?float4(actor.xy,ground,0):float4(0,0,0,0);
 float dt=max(timing.x,.001);
 float2 delta=detected?actor.xy-old.xy:float2(0,0);
 float valid=step(length(delta),12);
 float blend=1-exp(-dt*6);
 float2 position=old.xy+delta*blend;
 float2 motion=lerp(velocity.xy,delta*blend/dt,1-exp(-dt*3))*valid;
 ground=old.z+clamp(ground-old.z,-dt*2,dt*2)*timing.z;
 float wake=lerp(old.w,saturate((length(motion)-.3)/3),1-exp(-dt*2));
 return uv.x<.5?float4(position,ground,wake):float4(motion,0,0);
})HLSL";

// Keep depth on the same ray as the integration UV. Averaging neighbouring
// hyperbolic depths moves a sloped receiver towards the camera and makes
// grazing ground fog systematically too thin, with periodic scanline gaps.
const char* kDepthSource = R"HLSL(
sampler2D fullDepth:register(s0);
float4 texel:register(c0);
float4 main(float2 uv:TEXCOORD0):COLOR0 {return tex2D(fullDepth,uv).rrrr;}
)HLSL";

// A world-space patch with four terrain-relative height slices packed in RGBA.
// Re-evaluated analytically each frame: no camera advection, history diffusion,
// readback, or dependence on actor motion. All expensive deformation lives here.
constexpr UINT kLocalFogFieldSize = 256;
constexpr float kLocalFogFieldExtent = 256.f;
const char* kLocalFogFieldSource = R"HLSL(
sampler2D noiseMap:register(s0); sampler2D groundState:register(s1);
float4 wakeShape:register(c1); // radius, trail length, strength
float4 fieldOrigin:register(c0); // lower-left XY, extent, time
float Noise3(float3 p) {
 // Smooth interpolation between decorrelated height slices in the noise atlas.
 float z=floor(p.z),f=frac(p.z);f=f*f*(3-2*f);
 float2 offset=float2(.173,.317);
 float a=tex2Dlod(noiseMap,float4(p.xy+z*offset,0,0)).r;
 float b=tex2Dlod(noiseMap,float4(p.xy+(z+1)*offset,0,0)).r;
 return lerp(a,b,f);
}
float Bank(float3 p,float center,float thickness,float seed) {
 float t=fieldOrigin.w;
 float3 flow=float3(t*.003,-t*.002,t*.004);
 float broad=Noise3(p+flow+seed);
 float detail=Noise3(p*2.13-flow*.63+seed+.37);
 float shape=smoothstep(.28,.72,broad*.78+detail*.22);
 float top=center+(detail-.5)*.3;
 return shape*exp(-2*(p.z-top)*(p.z-top)/(thickness*thickness));
}
float Density(float2 world,float height) {
 // Independent overlapping volumes: changing flow deforms bridges between banks.
 float3 p=float3(world/42,height);
 float low=Bank(p,0,.65,0);
 float middle=Bank(float3(p.xy*1.37,p.z),.48,.62,31.4);
 float upper=Bank(float3(p.xy*.73,p.z),.95,.58,67.9);
 return 1-exp(-(low*.85+middle*.65+upper*.4)*1.25);
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float2 world=fieldOrigin.xy+uv*fieldOrigin.z;
 float4 density=0;
 [loop]for(int slice=0;slice<4;++slice){float v=Density(world,slice*.5);density+=v*float4(slice==0,slice==1,slice==2,slice==3);}
 float4 actor=tex2Dlod(groundState,float4(.25,.5,0,0));
 float2 velocity=tex2Dlod(groundState,float4(.75,.5,0,0)).xy;
 float2 rel=world-actor.xy,flowDir=normalize(velocity+float2(1e-4,0));
 float radius=max(wakeShape.x,.1);
 float along=dot(rel,flowDir),side=dot(rel,float2(-flowDir.y,flowDir.x));
 float2 front=rel-flowDir*radius*.45;
 float body=exp(-dot(front,front)/(radius*radius))*(1-smoothstep(radius,2*radius,length(front)));
 float behind=smoothstep(-wakeShape.y,-radius*.25,along)*(1-smoothstep(radius*.25,radius,along));
 float trail=exp(-side*side/(radius*radius*.58))*behind*step(.25,dot(velocity,velocity))*(1-smoothstep(radius,2*radius,abs(side)));
 float wake=actor.w*wakeShape.z;
 float clearing=saturate(max(body,trail)*wake);
 float eddy=sin(along*1.3-fieldOrigin.w*.55)*sin(side*1.6)*trail*wake;
 return density*(1-clearing*.94)*max(0,1+eddy*.22);
})HLSL";

const char* kIntegrateSource = R"HLSL(
sampler2D depthMap:register(s0); sampler2D noiseMap:register(s1); sampler2D groundState:register(s2); sampler2D waterCoverage:register(s3); sampler2D terrainHeight:register(s4);
sampler2D localField:register(s5);
float4 cameraPos:register(c0); float4 celestial:register(c1);
float4 ambientAerial:register(c2); float4 directExtinction:register(c3);
float4 projection:register(c4);
float4 invView0:register(c5); float4 invView1:register(c6); float4 invView2:register(c7);
float4 distanceTuning:register(c8); float4 heightTuning:register(c9);
float4 mistTuning:register(c10); float4 noiseTuning:register(c11);
float4 phaseTuning:register(c12); float4 frameTuning:register(c13);
float4 sourceScreen:register(c14); // xy=confirmed disc UV, z=aspect, w=valid
float4 localFog:register(c15); // x=enabled,y=density,z=terrain-relative offset,w=height falloff
float4 localShape:register(c17); // y=local range; wake is evaluated in the field pass
float4 localLightPosRadius[8]:register(c18);
float4 localLightColorPower[8]:register(c26);
float4 localLightDebug:register(c34); // x=debug,y=light count
float4 layerEnable:register(c35); // x=global enabled,y=density scale,z=daylight,w=moonlight
float4 terrainOrigin:register(c36); // center XY, height origin, reciprocal extent
float4 terrainControl:register(c37); // valid
float4 fieldOrigin:register(c38); // lower-left XY, reciprocal extent
float2 Ground(float3 p,float fallback) {
 float2 q=(p.xy-terrainOrigin.xy)*terrainOrigin.w+.5;
 float2 h=tex2Dlod(terrainHeight,float4(q,0,0)).rg;
 float coverage=smoothstep(0,1,h.y)*(1-smoothstep(.47,.495,max(abs(q.x-.5),abs(q.y-.5))))*terrainControl.x;
 return float2(lerp(fallback,h.x/max(h.y,.001)+terrainOrigin.z,coverage),coverage);
}
float2 noiseAt(float3 p) {
 float2 wind=float2(frameTuning.x*.002,-frameTuning.x*.0012);
 float large=tex2Dlod(noiseMap,float4((p.xy+p.z*float2(.04,.03))*noiseTuning.x+wind,0,0)).r;
 float small=tex2Dlod(noiseMap,float4((p.xy+p.z*float2(-.03,.05))*noiseTuning.y-wind*1.7+.37,0,0)).g;
 return float2(large,small);
}
float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0 {
 float raw=tex2D(depthMap,uv).r;
 float sky=smoothstep(.99990,.99999,raw);
 float z=raw>=.9999?distanceTuning.y:projection.y/(raw-projection.x);
 z=clamp(z,0,distanceTuning.y);
 float3 view=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1);
 float viewScale=length(view); float3 viewRay=view/viewScale;
 float3 ray=normalize(viewRay.x*invView0.xyz+viewRay.y*invView1.xyz+viewRay.z*invView2.xyz);
 float marchDistance=min(z*viewScale,distanceTuning.y);
 float3 surface=cameraPos.xyz+ray*marchDistance;
 float4 actor=tex2Dlod(groundState,float4(.25,.5,0,0));
 float ground=lerp(actor.z,surface.z,step(.35,tex2D(waterCoverage,uv).r));
 float horizon=saturate(1-abs(ray.z)*1.65);
 int count=(int)distanceTuning.z; float stepLength=marchDistance/max((float)count,1);
 float jitter=frac(52.9829189*frac(dot(vpos,float2(.06711056,.00583715)))); // Fixed spatial stratification, never frame-counter noise.
 float cosTheta=dot(viewRay,normalize(celestial.xyz));
 float2 sourceDelta=float2((uv.x-sourceScreen.x)*sourceScreen.z,uv.y-sourceScreen.y);
 float sourceRadius=length(sourceDelta);
 float wideHalo=exp(-sourceRadius*sourceRadius*phaseTuning.x);
 float forwardHalo=exp(-sourceRadius*sourceRadius*phaseTuning.y);
 float screenPhase=(phaseTuning.z*wideHalo+phaseTuning.w*forwardHalo)*sourceScreen.w;
 float directionalLean=pow(saturate(cosTheta*.5+.5),6)*.08;
 float discProtection=lerp(.06,1,smoothstep(.025,.055,sourceRadius));
 // Normalized forward phase, bounded for artistic intensity and half-float stability.
 float g=.62;
 float angular=(1-g*g)/pow(max(1+g*g-2*g*cosTheta,.04),1.5);
 float phase=(angular*.12*phaseTuning.z+screenPhase*.12+directionalLean*.25)*discProtection;

 // The horizon layer is analytic and therefore cannot disappear because a
 // raymarch missed the ground. Density changes its strength while distance
 // remains the artistic reach control. Sky receives only the horizon part,
 // preserving the authored sky colour above the skyline.
 float distance01=saturate((marchDistance-distanceTuning.x)/max(distanceTuning.y-distanceTuning.x,1));
 float distanceCurve=distance01*distance01*(3-2*distance01);
 float aerialPath=max(0,marchDistance-distanceTuning.x);
 float globalOptical=layerEnable.x*(distanceCurve*2.15*layerEnable.y+
                     aerialPath*ambientAerial.w)*lerp(.72,1.18,horizon);
 globalOptical*=lerp(1,horizon*horizon,sky);
 globalOptical+=layerEnable.x*sky*horizon*.055*layerEnable.y;

 // The density field follows real world-space terrain/liquid heights at
 // EVERY sample. No shared screen-probed floor or hard horizontal slab.
 float heightOptical=0,localOptical=0,noiseSum=0,heightSum=0,mistSum=0;
 float3 localLightSum=0;
 float entry=0,exit=min(marchDistance,localShape.y*1.5);
 [loop] for(int i=0;i<96;++i) {
   if(i>=count) break;
   float u0=(float)i/count,u1=(float)(i+1)/count;
   float reach=max(0,exit-entry);
   float t0=entry+u0*reach,t1=entry+u1*reach;
   float segmentLength=t1-t0;
   float t=lerp(t0,t1,.2+jitter*.6); float3 p=cameraPos.xyz+ray*t;
   float2 noise=noiseAt(p);
   float broad=noise.x*.68+noise.y*.32;
   float bank=smoothstep(.30,.78,broad);
   float2 floor=Ground(p,ground);
   // Atlas coverage is confidence in the height, never an extinction mask.
   float coverage=smoothstep(-.4,.05,p.z-floor.x);
   float groundLayer=exp(-max(0,p.z-floor.x-heightTuning.x)*max(heightTuning.y,.025));
   float mistLayer=exp(-max(0,p.z-floor.x-mistTuning.x)*max(mistTuning.y,.08));
   float nearFade=smoothstep(1,6,t);
   float hd=layerEnable.x*heightTuning.z*groundLayer*smoothstep(18,55,t);
   float md=layerEnable.x*mistTuning.z*mistLayer*bank*nearFade;
   heightOptical+=(hd+md)*segmentLength*coverage;

   float localRange=1-smoothstep(localShape.y*.65,localShape.y,length(p.xy-cameraPos.xy));
   float height=max(0,p.z-floor.x-localFog.z)*localFog.w;
   float4 layers=tex2Dlod(localField,float4((p.xy-fieldOrigin.xy)*fieldOrigin.z,0,0));
   float localBank=dot(layers,saturate(1-abs(height-float4(0,.5,1,1.5))*2))*2.2;
   float ld=localFog.x*localFog.y*localBank*localRange*nearFade*smoothstep(-.4,.05,p.z-floor.x-localFog.z);
   localOptical+=ld*segmentLength*coverage;

   heightSum+=hd;mistSum+=md;noiseSum+=bank;
 }
 // Integrate each lamp over its actual ray/sphere intersection. A small
 // lantern must not vanish between the 4..8 samples of the world fog march.
 // Clipping the interval to scene depth keeps foreground walls opaque.
 [loop] for(int li=0;li<8;++li) {
   if(li>=(int)localLightDebug.y) break;
   float3 toLight=localLightPosRadius[li].xyz-cameraPos.xyz;
   float radius=max(localLightPosRadius[li].w,.01);
   float along=dot(toLight,ray);
   float perpendicular2=max(0,dot(toLight,toLight)-along*along);
   float halfChord=sqrt(max(0,radius*radius-perpendicular2));
   float begin=max(0,along-halfChord),end=min(marchDistance,along+halfChord);
   float interval=max(0,end-begin);
   float closest=clamp(along,begin,max(begin,end));
   float3 p=cameraPos.xyz+ray*closest;
   float2 floor=Ground(p,ground);
   // Thin ambient aerosol also exists above the ground banks. Its small
   // optical depth is only visible near a real emitter, not as screen wash.
   float medium=layerEnable.x*ambientAerial.w+localFog.x*localFog.y*.06;
   medium+=localFog.x*localFog.y*.25*exp(-max(0,p.z-floor.x-localFog.z)*localFog.w);
   float edge=saturate(1-perpendicular2/(radius*radius));
   float scatter=(1-exp(-medium*interval*directExtinction.w))*edge*edge/(1+perpendicular2*.035);
   float transmission=exp(-ambientAerial.w*layerEnable.x*begin*directExtinction.w);
   localLightSum+=pow(max(localLightColorPower[li].rgb,0),2.2)*localLightColorPower[li].w*scatter*transmission;
 }
 float optical=max(0,globalOptical+heightOptical+localOptical)*directExtinction.w;
 float T=exp(-optical); float fogAmount=1-T;
 float day=saturate(layerEnable.z),moon=saturate(layerEnable.w);
 float3 neutralFog=pow(max(ambientAerial.rgb,0),2.2)*lerp(.55,1,saturate(day+moon*.65));
 // Global wash controls distant haze only. Ground banks retain their own
 // neutral ambient scattering, including at night and away from lamps.
 float localAmount=1-exp(-max(0,localOptical)*directExtinction.w);
 float globalAmount=(1-exp(-max(0,globalOptical+heightOptical)*directExtinction.w))*frameTuning.z;
 T=(1-globalAmount)*(1-localAmount);
 float3 ambientSum=neutralFog*(globalAmount*(1-localAmount)+localAmount);
 float3 directSum=pow(max(directExtinction.rgb,0),2.2)*phase*celestial.w*fogAmount*frameTuning.z;
 // Light Rays controls lamp scattering independently of global Fog Wash.
 float invCount=1/max((float)count,1); int debugMode=(int)frameTuning.y;
 if(debugMode==1)return optical.xxxx; if(debugMode==2)return T.xxxx;
 if(debugMode==3)return saturate((globalOptical+heightOptical+localOptical)*.5).xxxx;
 if(debugMode==4)return (heightSum*invCount*25).xxxx;
 if(debugMode==5)return (mistSum*invCount*25).xxxx;
 if(debugMode==6)return (noiseSum*invCount).xxxx;
 if(debugMode==7)return float4(ambientSum,1-T);
 if(debugMode==8)return float4(directSum,1-T);
 if(localLightDebug.x>3.5)return float4(localLightSum,1);
 return float4(ambientSum+directSum+localLightSum,1-T);
})HLSL";

const char* kTemporalSource = R"HLSL(
sampler2D currentMap:register(s0); sampler2D historyMap:register(s1);
sampler2D currentDepth:register(s2); sampler2D historyDepth:register(s3);
float4 temporal:register(c0); float4 projection:register(c1);
float4 inv0:register(c2); float4 inv1:register(c3); float4 inv2:register(c4); float4 camera:register(c5);
float4 prev0:register(c6); float4 prev1:register(c7); float4 prev2:register(c8); float4 prev3:register(c9);
float4 prevProjection:register(c10);
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float4 cur=tex2D(currentMap,uv); if(temporal.w<.5)return cur;
 float raw=tex2D(currentDepth,uv).r; float sky=step(.9999,raw); float2 prevUv;
 if(sky>.5) {
   float3 vr=normalize(float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1));
   float3 wr=normalize(vr.x*inv0.xyz+vr.y*inv1.xyz+vr.z*inv2.xyz);
   // Row-vector D3D convention: world * view. The shader constants contain
   // matrix rows, therefore each output component is one matrix column.
   float3 pv=float3(
     wr.x*prev0.x+wr.y*prev1.x+wr.z*prev2.x,
     wr.x*prev0.y+wr.y*prev1.y+wr.z*prev2.y,
     wr.x*prev0.z+wr.y*prev1.z+wr.z*prev2.z);
   if(pv.z<=.02)return cur;
   prevUv=float2(.5+.5*pv.x/pv.z*prevProjection.z,.5-.5*pv.y/pv.z*prevProjection.w);
   // Never pull bright sky history across a moving geometry silhouette.
   if(tex2D(historyDepth,prevUv).r<.9999)return cur;
 } else {
   float z=projection.y/(raw-projection.x);
   float3 vp=float3((uv.x*2-1)/projection.z,(1-uv.y*2)/projection.w,1)*z;
   float3 wp=camera.xyz+vp.x*inv0.xyz+vp.y*inv1.xyz+vp.z*inv2.xyz;
   float3 pv=float3(
     wp.x*prev0.x+wp.y*prev1.x+wp.z*prev2.x+prev3.x,
     wp.x*prev0.y+wp.y*prev1.y+wp.z*prev2.y+prev3.y,
     wp.x*prev0.z+wp.y*prev1.z+wp.z*prev2.z+prev3.z);
   if(pv.z<=.02)return cur;
   prevUv=float2(.5+.5*pv.x/pv.z*prevProjection.z,.5-.5*pv.y/pv.z*prevProjection.w);
   float oldRaw=tex2D(historyDepth,prevUv).r;
   float oldZ=oldRaw>=.9999?100000:prevProjection.y/(oldRaw-prevProjection.x);
   if(abs(oldZ-pv.z)>max(2.0,pv.z*.035))return cur;
 }
 if(any(prevUv<0)||any(prevUv>1))return cur;
 float2 ts=temporal.xy; float4 lo=cur,hi=cur,v;
 v=tex2D(currentMap,uv+float2(ts.x,0));lo=min(lo,v);hi=max(hi,v);
 v=tex2D(currentMap,uv-float2(ts.x,0));lo=min(lo,v);hi=max(hi,v);
 v=tex2D(currentMap,uv+float2(0,ts.y));lo=min(lo,v);hi=max(hi,v);
 v=tex2D(currentMap,uv-float2(0,ts.y));lo=min(lo,v);hi=max(hi,v);
 return lerp(cur,clamp(tex2D(historyMap,prevUv),lo,hi),temporal.z);
})HLSL";

const char* kUpsampleSource = R"HLSL(
sampler2D lowMap:register(s0); sampler2D fullDepth:register(s1); sampler2D lowDepth:register(s2);
float4 texels:register(c0); float4 projection:register(c1); float4 tuning:register(c2);
float Linear(float r){return r>=.9999?projection.z:projection.y/(r-projection.x);}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float2 o[4]={float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
 float2 lowSize=1/texels.zw;
 float2 lowPixel=uv*lowSize-.5;
 float2 base=floor(lowPixel),fraction=frac(lowPixel);
 float full=Linear(tex2D(fullDepth,uv).r),sum=0; float4 result=0; float best=1e9; float4 nearest=0;
 float zl=Linear(tex2D(fullDepth,uv-float2(texels.x,0)).r),zr=Linear(tex2D(fullDepth,uv+float2(texels.x,0)).r);
 float zu=Linear(tex2D(fullDepth,uv-float2(0,texels.y)).r),zd=Linear(tex2D(fullDepth,uv+float2(0,texels.y)).r);
 float slope=min(abs(zl-full),abs(zr-full))+min(abs(zu-full),abs(zd-full));
 float tolerance=max(max(.35,full*.012),slope*max(texels.z/texels.x,texels.w/texels.y)*1.5);
 [unroll]for(int i=0;i<4;++i){
   float2 q=(base+o[i]+.5)*texels.zw;
   float z=Linear(tex2D(lowDepth,q).r); float d=abs(full-z);
   float2 axisWeight=1-abs(o[i]-fraction);
   float spatial=max(axisWeight.x*axisWeight.y,.001);
   float w=spatial*exp(-d/tolerance);
   float4 c=tex2D(lowMap,q); result+=c*w; sum+=w;
   if(d<best){best=d;nearest=c;}
 }
 result=sum>.001?result/sum:nearest;
 if(tuning.x>10.5)return float4(result.rgb,1); return result;
})HLSL";

const char* kCompositeSource = R"HLSL(
sampler2D sceneMap:register(s0); sampler2D atmosphereMap:register(s1); sampler2D depthMap:register(s2);
sampler2D waterMask:register(s3);
// Global wash is applied in integration, before combining ground banks.
// x=debug passthrough, y=1, z=extinction scale, w=scatter scale.
float4 tuning:register(c0); float4 sourceScreen:register(c1);
float4 edgeTuning:register(c3); // start distance, power, enabled
float4 edgeColor:register(c4);
float4 edgeProjection:register(c5);float4 worldUp:register(c6);
float4 projection:register(c2); // x=A,y=B,z=max atmosphere distance
float Linear(float raw){return raw>=.9999?projection.z:projection.y/(raw-projection.x);}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
 float4 a=tex2D(atmosphereMap,uv); if(tuning.x>.5)return float4(a.rgb,1);
 float4 scene=tex2D(sceneMap,uv);
 // Proper radiative form: T is the fraction of the scene that survives
 // extinction through the medium, not "how much fog to paint over the
 // screen". a.a is stored as (1-T) by the integrate pass.
 float T=saturate(1-a.a);
 float wash=tuning.y;
 float extinctionAmt=lerp(1.0,T,saturate(wash*tuning.z));
 float3 sceneLinear=pow(max(scene.rgb,0),2.2);
 float3 scattering=max(a.rgb*wash*tuning.w,0);
 // Compress added radiance using the remaining display headroom. At zero fog
 // the scene is exactly preserved; bright scatter approaches white smoothly.
 float3 transmitted=sceneLinear*extinctionAmt;
 float3 headroom=max(1-transmitted,.02);
 float3 result=transmitted+headroom*(1-exp(-scattering/headroom));
 float depth=tex2D(depthMap,uv).r;
 float edgeDistance=depth>=.9999?max(projection.z,edgeTuning.x*2):Linear(depth);
 float edge=saturate((edgeDistance-edgeTuning.x)/max(edgeTuning.x*.65,1));
 edge=(1-exp(-edge*edge*edgeTuning.y*3))*edgeTuning.z;
 float3 skyRay=normalize(float3((uv.x*2-1)/edgeProjection.x,(1-uv.y*2)/edgeProjection.y,1));
 float horizon=saturate(1-abs(dot(skyRay,worldUp.xyz))*2);
 edge*=depth>=.9999?horizon*horizon:1;
 result=lerp(result,pow(max(edgeColor.rgb,0),2.2),edge);
 // Boundary depth already clips the air integration to the water surface.
 // Preserve the game's own sun disc luminance. Atmosphere supplies the halo,
 // not a second emitter painted over the sprite. The sky-depth gate prevents
 // this protection mask from punching through foreground occluders.
 float2 delta=float2((uv.x-sourceScreen.x)*sourceScreen.z,uv.y-sourceScreen.y);
 float disc=(1-smoothstep(.022,.045,length(delta)))*sourceScreen.w;
 disc*=smoothstep(.9985,.9999,tex2D(depthMap,uv).r);
 result=lerp(result,sceneLinear,disc);
 result=pow(max(result,0),1/2.2);
 // No water-specific handling here any more. The integrate/upsample/
 // temporal pipeline now marches air only down to the water SURFACE (see
 // kBoundarySource), not the seabed, so its output over water is already
 // correct - distant water gets natural haze, nearby water isn't washed
 // out, and there's no separate on/off or distance-gradient hack needed.
 return float4(result,scene.a);
})HLSL";

bool Compile(IDirect3DDevice9* d,const char* source,IDirect3DPixelShader9** shader)
{
 ComPtr<ID3DBlob> blob,errors;
 if(FAILED(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.GetAddressOf(),errors.GetAddressOf()))||!blob)return false;
 return SUCCEEDED(d->CreatePixelShader(static_cast<DWORD*>(blob->GetBufferPointer()),shader));
}
}

DirectionalVolumetricLighting& DirectionalVolumetricLighting::Instance(){static DirectionalVolumetricLighting v;return v;}

void DirectionalVolumetricLighting::Configure(const std::wstring& basePath)
{
 const std::wstring ini=basePath+L"GraphicsEffects.ini";
 m_logPath=basePath+L"Atmosphere.log";
 auto read=[&](const wchar_t* key,int fallback)->int{return static_cast<int>(renderer::locationtuning::ReadInt(L"Atmosphere",key,fallback,ini.c_str()));};
 m_settings.enabled=read(L"DirectionalVolumetricEnabled",1)!=0;
 m_settings.quality=static_cast<uint32_t>(std::clamp(read(L"AtmosphereQuality",1),0,2));
 static constexpr uint32_t samples[]={12,16,24}; static constexpr float scales[]={.25f,.25f,.5f};
 m_settings.sampleCount=samples[m_settings.quality];m_settings.resolutionScale=scales[m_settings.quality];
 m_settings.densityScale=std::clamp(read(L"DensityPermille",5),0,20)/5.f;
 m_settings.maxDistance=float(std::clamp(read(L"AtmosphereMaxDistance",520),120,1200));
 m_settings.aerialStart=float(std::clamp(read(L"AerialStartDistance",35),0,300));
 m_settings.aerialDensity=std::clamp(read(L"AerialDensityPermille",2),0,20)*.001f;
 m_settings.heightDensity=std::clamp(read(L"HeightDensityPermille",5),0,30)*.001f;
 m_settings.heightFalloff=std::clamp(read(L"HeightFalloffPermille",55),5,500)*.001f;
 m_settings.fogBaseOffset=float(std::clamp(read(L"FogBaseOffset",-6),-100,100));
 m_settings.mistBaseOffset=float(std::clamp(read(L"GroundMistOffset",-3),-50,50));
 m_settings.mistDensity=std::clamp(read(L"GroundMistDensityPermille",9),0,40)*.001f;
 m_settings.mistFalloff=std::clamp(read(L"GroundMistFalloffPermille",420),20,2000)*.001f;
 m_settings.noiseAmount=std::clamp(read(L"AtmosphereNoisePercent",22),0,30)*.01f;
 m_settings.extinction=std::clamp(read(L"AtmosphereExtinctionPercent",100),20,200)*.01f;
 m_settings.moonStrength=std::clamp(read(L"MoonVolumetricStrengthPercent",16),0,40)*.01f;
 m_settings.temporalEnabled=read(L"AtmosphereTemporalEnabled",1)!=0;
 m_settings.temporalBlend=std::clamp(read(L"AtmosphereTemporalPercent",88),0,95)*.01f;
 // Same keys the tuning overlay's SUN GLOW / FOG WASH sliders write - this
 // is the pipeline that actually renders the sun halo and the composited
 // fog now, so it must be the one reading them, not the retired full-res
 // analytic shader these keys used to belong to.
 m_settings.sunGlowStrength=std::clamp(read(L"SunGlowPercent",80),0,300)*.01f;
 // Deliberately NOT the old "FogWashPercent" key - that one is still on
 // disk in deployed GraphicsEffects.ini copies at values tuned for the
 // retired full-res analytic shader (e.g. 8), which would read here as
 // "almost no fog" and silently gut this pass. New key, own default.
 m_settings.fogWash=std::clamp(read(L"AtmosphereWashPercent",55),0,100)*.01f;
 // ROUND 5 Phase 16-17. Not exposed in the tuning overlay - deliberate
 // internal knobs for now, defaults keep the old single-wash behaviour.
 m_settings.extinctionStrength=std::clamp(read(L"AtmosphereExtinctionPercentInternal",100),0,300)*.01f;
 m_settings.scatterStrength=std::clamp(read(L"AtmosphereScatterPercentInternal",100),0,300)*.01f;
 // Default off: live testing showed a case where this fogged the whole
 // sky (sun invisible even at zenith, independent of in-game weather) -
 // now restricted to the one register with real confidence (water), but
 // defaulting off until that narrower version is confirmed safe across
 // more zones. The toggle in the overlay still works for testing it.
 m_settings.useEnvironmentBaseline=read(L"UseEnvironmentFog",0)!=0;
 m_settings.debugMode=static_cast<VolumetricDebugMode>(std::clamp(read(L"AtmosphereDebugMode",0),0,12));
 const auto readLocal=[&](const wchar_t* key,int fallback)->int{return static_cast<int>(renderer::locationtuning::ReadInt(L"LocalFog",key,fallback,ini.c_str()));};
 m_settings.localFogEnabled=readLocal(L"Enabled",1)!=0;
 m_settings.localFogDensity=std::clamp(readLocal(L"DensityPermille",12),0,100)*.003f;
 // Height is now a real thickness in world units: higher means taller.
 m_settings.localFogHeightFalloff=1.f/std::clamp(readLocal(L"HeightUnits",3),1,8);
 m_settings.localFogBaseOffset=float(std::clamp(readLocal(L"BaseOffset",-1),-20,20));
 m_settings.localFogWakeStrength=std::clamp(readLocal(L"WakeStrengthPercent",85),0,150)*.01f;
 m_settings.localFogWakeRadius=float(std::clamp(readLocal(L"WakeRadius",5),2,14));
 m_settings.localFogTrailLength=float(std::clamp(readLocal(L"TrailLength",18),4,40));
 m_settings.edgeFogEnabled=renderer::locationtuning::ReadInt(L"DistanceFog",L"Enabled",1,ini.c_str())!=0;
 m_settings.edgeFogDistance=float(std::clamp(int(renderer::locationtuning::ReadInt(L"DistanceFog",L"DistancePercent",130,ini.c_str())),10,300));
 m_settings.edgeFogPower=std::clamp(int(renderer::locationtuning::ReadInt(L"DistanceFog",L"PowerPercent",30,ini.c_str())),10,300)*.01f;
 m_historyValid=false;
}

void DirectionalVolumetricLighting::Reset(IDirect3DDevice9* device)
{
 if(m_owner&&m_owner!=device)return;
 m_integratedSurface.Reset();m_integratedTexture.Reset();m_upsampledSurface.Reset();m_upsampledTexture.Reset();
 m_boundarySurface.Reset();m_boundaryTexture.Reset();
 m_localFogFieldSurface.Reset();m_localFogFieldTexture.Reset();m_localFogFieldShader.Reset();
 for(int i=0;i<2;++i){m_historySurface[i].Reset();m_historyTexture[i].Reset();m_depthSurface[i].Reset();m_depthTexture[i].Reset();m_groundHeightSurface[i].Reset();m_groundHeightTexture[i].Reset();m_groundHeightIssued[i]=false;}

 m_depthShader.Reset();m_integrateShader.Reset();m_temporalShader.Reset();m_upsampleShader.Reset();m_compositeShader.Reset();m_boundaryShader.Reset();m_boundaryDebugShader.Reset();m_groundHeightShader.Reset();
 m_owner=nullptr;m_fullWidth=m_fullHeight=m_lowWidth=m_lowHeight=0;m_targetFormat=D3DFMT_UNKNOWN;m_historyValid=false;m_previousViewValid=false;
 m_previousLocalLightSignature=0;
}

bool DirectionalVolumetricLighting::EnsureShaders(IDirect3DDevice9* d)
{
 if(m_depthShader&&m_integrateShader&&m_temporalShader&&m_upsampleShader&&m_compositeShader&&m_boundaryShader&&m_boundaryDebugShader&&m_groundHeightShader&&m_localFogFieldShader)return true;
 return Compile(d,kDepthSource,m_depthShader.GetAddressOf())&&Compile(d,kIntegrateSource,m_integrateShader.GetAddressOf())&&Compile(d,kTemporalSource,m_temporalShader.GetAddressOf())&&Compile(d,kUpsampleSource,m_upsampleShader.GetAddressOf())&&Compile(d,kCompositeSource,m_compositeShader.GetAddressOf())&&Compile(d,kBoundarySource,m_boundaryShader.GetAddressOf())&&Compile(d,kBoundaryDebugSource,m_boundaryDebugShader.GetAddressOf())&&Compile(d,kGroundHeightSource,m_groundHeightShader.GetAddressOf())&&Compile(d,kLocalFogFieldSource,m_localFogFieldShader.GetAddressOf());
}

bool DirectionalVolumetricLighting::EnsureResources(IDirect3DDevice9* d,uint32_t w,uint32_t h,D3DFORMAT format)
{
 uint32_t lw=std::max(1u,uint32_t(w*m_settings.resolutionScale)),lh=std::max(1u,uint32_t(h*m_settings.resolutionScale));
 if(m_owner==d&&m_fullWidth==w&&m_fullHeight==h&&m_lowWidth==lw&&m_lowHeight==lh&&m_targetFormat==format&&m_upsampledTexture)return true;
 Reset(d);m_owner=d;m_fullWidth=w;m_fullHeight=h;m_lowWidth=lw;m_lowHeight=lh;m_targetFormat=format;
 D3DFORMAT hdr=D3DFMT_A16B16G16R16F;
 auto tex=[&](UINT tw,UINT th,D3DFORMAT f,ComPtr<IDirect3DTexture9>& t,ComPtr<IDirect3DSurface9>& s){return SUCCEEDED(d->CreateTexture(tw,th,1,D3DUSAGE_RENDERTARGET,f,D3DPOOL_DEFAULT,t.GetAddressOf(),nullptr))&&SUCCEEDED(t->GetSurfaceLevel(0,s.GetAddressOf()));};
 if(!tex(lw,lh,hdr,m_integratedTexture,m_integratedSurface)){hdr=D3DFMT_A8R8G8B8;if(!tex(lw,lh,hdr,m_integratedTexture,m_integratedSurface))return false;}
 for(int i=0;i<2;++i){if(!tex(lw,lh,hdr,m_historyTexture[i],m_historySurface[i]))return false;if(!tex(lw,lh,D3DFMT_R32F,m_depthTexture[i],m_depthSurface[i])&&!tex(lw,lh,D3DFMT_A8R8G8B8,m_depthTexture[i],m_depthSurface[i]))return false;}
 if(!tex(w,h,D3DFMT_R32F,m_boundaryTexture,m_boundarySurface)&&!tex(w,h,D3DFMT_A8R8G8B8,m_boundaryTexture,m_boundarySurface))return false;
 // Persistent world-height/actor state. GPU only; no staging/readbacks.
 for(int i=0;i<2;++i){
  m_groundHeightSurface[i].Reset();m_groundHeightTexture[i].Reset();
  if(!tex(2,1,D3DFMT_A32B32G32R32F,m_groundHeightTexture[i],m_groundHeightSurface[i]))return false;
  m_groundHeightIssued[i]=false;
 }

 if(!tex(kLocalFogFieldSize,kLocalFogFieldSize,hdr,m_localFogFieldTexture,m_localFogFieldSurface))return false;
 return tex(w,h,hdr,m_upsampledTexture,m_upsampledSurface);
}

void DirectionalVolumetricLighting::DrawScreenQuad(IDirect3DDevice9* d,uint32_t w,uint32_t h)
{
 struct V{float x,y,z,rhw,u,v;};float fw=float(w)-.5f,fh=float(h)-.5f;
 V q[]={{-.5f,-.5f,0,1,0,0},{fw,-.5f,0,1,1,0},{-.5f,fh,0,1,0,1},{fw,fh,0,1,1,1}};
 d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(V));
}

bool DirectionalVolumetricLighting::ValidateHistory(const FrameContext& f)
{
 if(!m_historyValid||!m_previousViewValid||!f.previousViewValid||m_previousWasMoon!=f.celestialIsMoon||std::abs(m_previousCelestialIntensity-f.celestialIntensity)>.5f||m_previousLocalLightSignature!=LocalLightManager::Instance().SelectionSignature())return false;
 float dx=f.cameraPosition.x-m_previousCamera.x,dy=f.cameraPosition.y-m_previousCamera.y,dz=f.cameraPosition.z-m_previousCamera.z;
 if(dx*dx+dy*dy+dz*dz>2500)return false;
 for(int i=0;i<4;++i)if(std::abs(f.projUnpack[i]-m_previousProjection[i])>.01f*std::max(1.f,std::abs(m_previousProjection[i])))return false;
 return true;
}

bool DirectionalVolumetricLighting::Render(IDirect3DDevice9* d,const FrameContext& f,IDirect3DSurface9* target,bool globalFogEnabled)
{
 auto settings=m_settings;
 settings.densityScale*=f.environment[FogDensity];
 settings.localFogDensity*=f.environment[LocalFog];
 settings.scatterStrength*=f.environment[Atmosphere];
 settings.edgeFogPower*=f.environment[DistanceFogPower];
 if(!settings.enabled||!d||!target||!f.cameraValid||!f.depthAvailable||!f.sceneColor||!f.atmosphereNoise)return false;
 // Resource recreation resets shaders too; compile only after that reset.
 D3DSURFACE_DESC desc{};if(FAILED(target->GetDesc(&desc))||!EnsureResources(d,desc.Width,desc.Height,desc.Format)||!EnsureShaders(d))return false;
 if((++m_renderFrames%300)==0&&!m_logPath.empty()){
  std::ofstream out(std::filesystem::path(m_logPath),std::ios::app);
  out<<"render camera=("<<f.cameraPosition.x<<','<<f.cameraPosition.y<<','<<f.cameraPosition.z
     <<") densityScale="<<settings.densityScale<<" aerial="<<settings.aerialDensity
     <<" height="<<settings.heightDensity<<" mist="<<settings.mistDensity
     <<" localEnabled="<<(settings.localFogEnabled?1:0)<<" local="<<settings.localFogDensity
     <<" groundAtlas="<<(GroundSurfaceCapture::Instance().Texture()?1:0)<<" fogTime="<<m_fogElapsed
     <<" wash="<<settings.fogWash<<" selectedLights="<<LocalLightManager::Instance().Selected().size()<<'\n';
 }
 ScopedRenderState state(d);
 DepthCapture::Instance().RawSetDepth(d,nullptr);
 for(auto s:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE,D3DRS_ALPHABLENDENABLE})d->SetRenderState(s,FALSE);
 d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_COLORWRITEENABLE,0xF);d->SetVertexShader(nullptr);
 const uint32_t write=1-m_historyReadIndex;D3DVIEWPORT9 low{0,0,m_lowWidth,m_lowHeight,0,1},full{0,0,m_fullWidth,m_fullHeight,0,1};
 // WoW's own authored fog as the baseline (ROUND 5, Phase 11-15): read-
 // only capture of the game's real fog curve/colour, updated every
 // matched draw this frame regardless of shader family (terrain/model/
 // water all feed the same EnvironmentFogCapture state). FOG DISTANCE
 // still applies as a scale on top of the captured zone-authored distance
 // rather than replacing it, so the slider stays meaningful while zone
 // identity (Elwynn vs a coastal fog bank) comes through instead of one
 // fixed synthetic profile everywhere.
 const auto& envFog=renderer::EnvironmentFogCapture::Instance().State();
 bool useEnv=settings.useEnvironmentBaseline&&envFog.valid;
 float envScale=settings.maxDistance/520.f;
 float aerialStart=useEnv?envFog.startDistance*envScale:settings.aerialStart;
 float aerialEnd=useEnv?envFog.endDistance*envScale:settings.maxDistance;
 aerialEnd=std::max(aerialEnd,aerialStart+10.f);
 float ambR=(useEnv&&envFog.colorValid)?envFog.ambientFogColor[0]:.36f;
 float ambG=(useEnv&&envFog.colorValid)?envFog.ambientFogColor[1]:.46f;
 float ambB=(useEnv&&envFog.colorValid)?envFog.ambientFogColor[2]:.58f;
 const ULONGLONG tick=GetTickCount64();
 const float dt=m_lastFogTick?std::clamp(float(tick-m_lastFogTick)*.001f,.001f,.1f):1.f/60;
 m_lastFogTick=tick;m_fogElapsed+=dt;
 {
  // Boundary depth first: everything below (low-res downsample, upsample,
  // temporal) reads this instead of the raw scene depth, so water's own
  // surface - not the seabed behind it - is what the atmosphere treats as
  // "where air stops".
  ScopedGpuTimer timer(d,GpuPerfStage::AtmosphereBoundary);
  d->SetViewport(&full);d->SetRenderTarget(0,m_boundarySurface.Get());d->SetPixelShader(m_boundaryShader.Get());
  d->SetTexture(0,f.depthTexture);d->SetTexture(1,f.waterMaskTexture);d->SetTexture(2,f.waterDepthTexture);
  for(DWORD s=0;s<3;++s){d->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_POINT);}
  float boundaryProjection[4]={f.projUnpack[0],f.projUnpack[1],f.viewport.MinZ,
                              f.depthMaxZ-f.viewport.MinZ};
  d->SetPixelShaderConstantF(0,boundaryProjection,1);
  DrawScreenQuad(d,m_fullWidth,m_fullHeight);
  d->SetTexture(0,nullptr);d->SetTexture(1,nullptr);d->SetTexture(2,nullptr);
 }
 // Persist ground and actor estimates on the GPU; bounded height changes
 // prevent a tree entering a probe from moving the whole fog layer at once.
 const uint32_t groundWrite=m_groundHeightWriteIndex,groundRead=1-groundWrite;
 {
  D3DVIEWPORT9 stateViewport{0,0,2,1,0,1};d->SetViewport(&stateViewport);
  d->SetRenderTarget(0,m_groundHeightSurface[groundWrite].Get());d->SetPixelShader(m_groundHeightShader.Get());
  d->SetTexture(0,m_boundaryTexture.Get());d->SetTexture(1,m_groundHeightTexture[groundRead].Get());
  for(DWORD si=0;si<2;++si){d->SetSamplerState(si,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(si,D3DSAMP_MAGFILTER,D3DTEXF_POINT);}
  float cam[4]={f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,0};
  float inv[3][4]={{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},{f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},{f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0}};
  bool stateValid=m_groundHeightIssued[groundRead]&&Length(f.cameraPosition-m_previousCamera)<30;
  float tracking=stateValid?std::clamp(Length(f.cameraPosition-m_previousCamera)/dt,0.f,1.f):1.f;
  float time[4]={dt,stateValid?1.f:0.f,tracking,0};
  d->SetPixelShaderConstantF(0,cam,1);d->SetPixelShaderConstantF(1,f.projUnpack,1);d->SetPixelShaderConstantF(2,inv[0],3);d->SetPixelShaderConstantF(5,time,1);
  DrawScreenQuad(d,2,1);d->SetTexture(0,nullptr);d->SetTexture(1,nullptr);
  m_groundHeightIssued[groundWrite]=true;m_groundHeightWriteIndex=groundRead;d->SetViewport(&full);
 }
 if(settings.debugMode==VolumetricDebugMode::BoundaryDepth){
  d->SetRenderTarget(0,target);d->SetPixelShader(m_boundaryDebugShader.Get());d->SetTexture(0,m_boundaryTexture.Get());
  d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  float c0[4]={f.projUnpack[0],f.projUnpack[1],aerialEnd,0};d->SetPixelShaderConstantF(0,c0,1);
  DrawScreenQuad(d,m_fullWidth,m_fullHeight);d->SetTexture(0,nullptr);
  return true;
 }
 // Snap to whole field texels so camera motion never swims the density grid.
 float field[4]={std::floor(f.cameraPosition.x)-kLocalFogFieldExtent*.5f,
                 std::floor(f.cameraPosition.y)-kLocalFogFieldExtent*.5f,kLocalFogFieldExtent,m_fogElapsed};
 if(settings.localFogEnabled && settings.localFogDensity>0){
  d->SetRenderTarget(0,m_localFogFieldSurface.Get());
  D3DVIEWPORT9 fieldViewport{0,0,kLocalFogFieldSize,kLocalFogFieldSize,0,1};d->SetViewport(&fieldViewport);
  d->SetPixelShader(m_localFogFieldShader.Get());d->SetTexture(0,f.atmosphereNoise);
  d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
  d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
  d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);
  d->SetTexture(1,m_groundHeightTexture[groundWrite].Get());
  d->SetSamplerState(1,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(1,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  d->SetSamplerState(1,D3DSAMP_MIPFILTER,D3DTEXF_NONE);d->SetSamplerState(1,D3DSAMP_SRGBTEXTURE,FALSE);
  d->SetSamplerState(1,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(1,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  float wakeShape[4]={settings.localFogWakeRadius,settings.localFogTrailLength,settings.localFogWakeStrength,0};
  d->SetPixelShaderConstantF(0,field,1);d->SetPixelShaderConstantF(1,wakeShape,1);
  DrawScreenQuad(d,kLocalFogFieldSize,kLocalFogFieldSize);d->SetTexture(0,nullptr);d->SetTexture(1,nullptr);
 }
 d->SetRenderTarget(0,m_depthSurface[write].Get());d->SetViewport(&low);d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);d->SetPixelShader(m_depthShader.Get());d->SetTexture(0,m_boundaryTexture.Get());float depthTexel[4]={.5f/m_fullWidth,.5f/m_fullHeight,0,0};d->SetPixelShaderConstantF(0,depthTexel,1);DrawScreenQuad(d,m_lowWidth,m_lowHeight);d->SetTexture(0,nullptr);
 {
  ScopedGpuTimer timer(d,GpuPerfStage::AtmosphereIntegrate);d->SetRenderTarget(0,m_integratedSurface.Get());d->SetPixelShader(m_integrateShader.Get());d->SetTexture(0,m_depthTexture[write].Get());d->SetTexture(1,f.atmosphereNoise);d->SetTexture(2,m_groundHeightTexture[groundWrite].Get());d->SetTexture(3,f.waterMaskTexture);d->SetSamplerState(3,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(3,D3DSAMP_MAGFILTER,D3DTEXF_POINT);d->SetSamplerState(2,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(2,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  for(DWORD s=0;s<2;++s){d->SetSamplerState(s,D3DSAMP_MINFILTER,s?D3DTEXF_LINEAR:D3DTEXF_POINT);d->SetSamplerState(s,D3DSAMP_MAGFILTER,s?D3DTEXF_LINEAR:D3DTEXF_POINT);d->SetSamplerState(s,D3DSAMP_ADDRESSU,s?D3DTADDRESS_WRAP:D3DTADDRESS_CLAMP);d->SetSamplerState(s,D3DSAMP_ADDRESSV,s?D3DTADDRESS_WRAP:D3DTADDRESS_CLAMP);}
  float c0[4]={f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,1};d->SetPixelShaderConstantF(0,c0,1);
  float intensity=f.celestialIntensity*(f.celestialIsMoon?settings.moonStrength:1.f);float c1[4]={f.sunDirectionView.x,f.sunDirectionView.y,f.sunDirectionView.z,intensity};d->SetPixelShaderConstantF(1,c1,1);
  float c2[4]={ambR,ambG,ambB,settings.aerialDensity*settings.densityScale};float c3[4]={std::clamp(ambR*1.5f,.12f,1.f),std::clamp(ambG*1.5f,.12f,1.f),std::clamp(ambB*1.5f,.12f,1.f),settings.extinction};d->SetPixelShaderConstantF(2,c2,1);d->SetPixelShaderConstantF(3,c3,1);d->SetPixelShaderConstantF(4,f.projUnpack,1);
  float inv[3][4]={{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},{f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},{f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0}};d->SetPixelShaderConstantF(5,inv[0],3);
  // The integration shader uses the persistent GPU ground reference.
  float c8[4]={aerialStart,aerialEnd,float(std::min(96u,settings.sampleCount*4)),0};float c9[4]={settings.fogBaseOffset,settings.heightFalloff,settings.heightDensity*settings.densityScale,0};float c10[4]={settings.mistBaseOffset,settings.mistFalloff,settings.mistDensity*settings.densityScale,settings.noiseAmount};float c11[4]={1.f/42.f,1.f/13.f,.30f,.18f};float c12[4]={32.f,220.f,.25f*settings.sunGlowStrength,.75f*settings.sunGlowStrength};float c13[4]={m_fogElapsed,float(uint32_t(settings.debugMode)),settings.fogWash,0};float c14[4]={f.sunScreenX,f.sunScreenY,float(m_fullWidth)/float(m_fullHeight),f.celestialIntensity>0.f?1.f:0.f};
  float c15[4]={settings.localFogEnabled?1.f:0.f,settings.localFogDensity,settings.localFogBaseOffset,settings.localFogHeightFalloff};
  float c17[4]={0,65.f,0,0};
  float localPosRadius[8][4]{};float localColorPower[8][4]{};
  const auto& localManager=LocalLightManager::Instance();const auto& localLights=localManager.Selected();
  std::vector<LocalLightSource> fogLights;for(const auto& light:localLights)if(light.flags & LocalLightVolumetric)fogLights.push_back(light);
  const int localCount=std::min<int>(8,int(fogLights.size()));
  for(int li=0;li<localCount;++li){localPosRadius[li][0]=fogLights[li].position.x;localPosRadius[li][1]=fogLights[li].position.y;localPosRadius[li][2]=fogLights[li].position.z;localPosRadius[li][3]=fogLights[li].radius;localColorPower[li][0]=fogLights[li].color.x;localColorPower[li][1]=fogLights[li].color.y;localColorPower[li][2]=fogLights[li].color.z;localColorPower[li][3]=fogLights[li].intensity*localManager.IntensityScale()*f.environment[Lighting]*std::sqrt(std::max(0.f,localManager.RayScale()));}
  float localDebug[4]={float(localManager.DebugMode()),float(localCount),0,0};
  // Keep the authored distance/height atmosphere and the local bank as
  // independent layers.  Day/night factors only tint the medium; they never
  // decide whether it exists.
  float layerEnable[4]={globalFogEnabled?1.f:0.f,settings.densityScale,
                        f.daylightFactor,f.moonlightFactor};
  d->SetPixelShaderConstantF(8,c8,1);d->SetPixelShaderConstantF(9,c9,1);d->SetPixelShaderConstantF(10,c10,1);d->SetPixelShaderConstantF(11,c11,1);d->SetPixelShaderConstantF(12,c12,1);d->SetPixelShaderConstantF(13,c13,1);d->SetPixelShaderConstantF(14,c14,1);d->SetPixelShaderConstantF(15,c15,1);d->SetPixelShaderConstantF(17,c17,1);d->SetPixelShaderConstantF(18,localPosRadius[0],8);d->SetPixelShaderConstantF(26,localColorPower[0],8);d->SetPixelShaderConstantF(34,localDebug,1);d->SetPixelShaderConstantF(35,layerEnable,1);
  const auto& terrain=GroundSurfaceCapture::Instance();
  float terrainControl[4]={terrain.Texture()?1.f:0.f,0,0,0};
  d->SetTexture(4,terrain.Texture());d->SetPixelShaderConstantF(36,terrain.Origin(),1);d->SetPixelShaderConstantF(37,terrainControl,1);
  d->SetSamplerState(4,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);d->SetSamplerState(4,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);d->SetSamplerState(4,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
  d->SetSamplerState(4,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(4,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);d->SetSamplerState(4,D3DSAMP_SRGBTEXTURE,FALSE);
  float fieldLookup[4]={field[0],field[1],1.f/kLocalFogFieldExtent,0};d->SetPixelShaderConstantF(38,fieldLookup,1);
  d->SetTexture(5,settings.localFogEnabled&&settings.localFogDensity>0?m_localFogFieldTexture.Get():nullptr);
  d->SetSamplerState(5,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);d->SetSamplerState(5,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
  d->SetSamplerState(5,D3DSAMP_MIPFILTER,D3DTEXF_NONE);d->SetSamplerState(5,D3DSAMP_SRGBTEXTURE,FALSE);
  d->SetSamplerState(5,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(5,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  DrawScreenQuad(d,m_lowWidth,m_lowHeight);for(DWORD si=0;si<6;++si)d->SetTexture(si,nullptr);
 }
 IDirect3DTexture9* atmosphere=m_integratedTexture.Get();bool historyOk=ValidateHistory(f);
 const bool runTemporal=settings.temporalEnabled&&(settings.debugMode==VolumetricDebugMode::None||settings.debugMode==VolumetricDebugMode::Temporal||settings.debugMode==VolumetricDebugMode::Upsampled);
 if(runTemporal){ScopedGpuTimer timer(d,GpuPerfStage::AtmosphereTemporal);d->SetRenderTarget(0,m_historySurface[write].Get());d->SetPixelShader(m_temporalShader.Get());d->SetTexture(0,m_integratedTexture.Get());d->SetTexture(1,m_historyTexture[m_historyReadIndex].Get());d->SetTexture(2,m_depthTexture[write].Get());d->SetTexture(3,m_depthTexture[m_historyReadIndex].Get());for(DWORD s=0;s<4;++s){d->SetSamplerState(s,D3DSAMP_MINFILTER,s<2?D3DTEXF_LINEAR:D3DTEXF_POINT);d->SetSamplerState(s,D3DSAMP_MAGFILTER,s<2?D3DTEXF_LINEAR:D3DTEXF_POINT);d->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);d->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);}float c0[4]={1.f/m_lowWidth,1.f/m_lowHeight,settings.temporalBlend,historyOk?1.f:0.f};d->SetPixelShaderConstantF(0,c0,1);d->SetPixelShaderConstantF(1,f.projUnpack,1);float inv[3][4]={{f.inverseView.m[0][0],f.inverseView.m[0][1],f.inverseView.m[0][2],0},{f.inverseView.m[1][0],f.inverseView.m[1][1],f.inverseView.m[1][2],0},{f.inverseView.m[2][0],f.inverseView.m[2][1],f.inverseView.m[2][2],0}};d->SetPixelShaderConstantF(2,inv[0],3);float cam[4]={f.cameraPosition.x,f.cameraPosition.y,f.cameraPosition.z,0};d->SetPixelShaderConstantF(5,cam,1);d->SetPixelShaderConstantF(6,&m_previousView.m[0][0],4);d->SetPixelShaderConstantF(10,m_previousProjection,1);DrawScreenQuad(d,m_lowWidth,m_lowHeight);for(DWORD s=0;s<4;++s)d->SetTexture(s,nullptr);atmosphere=m_historyTexture[write].Get();}
 {
  ScopedGpuTimer timer(d,GpuPerfStage::AtmosphereUpsample);d->SetViewport(&full);d->SetRenderTarget(0,m_upsampledSurface.Get());d->SetPixelShader(m_upsampleShader.Get());d->SetTexture(0,atmosphere);d->SetTexture(1,m_boundaryTexture.Get());d->SetTexture(2,m_depthTexture[write].Get());for(DWORD s=0;s<3;++s){d->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_POINT);}float c0[4]={1.f/m_fullWidth,1.f/m_fullHeight,1.f/m_lowWidth,1.f/m_lowHeight};float c1[4]={f.projUnpack[0],f.projUnpack[1],aerialEnd,0};float c2[4]={float(uint32_t(settings.debugMode)),0,0,0};d->SetPixelShaderConstantF(0,c0,1);d->SetPixelShaderConstantF(1,c1,1);d->SetPixelShaderConstantF(2,c2,1);DrawScreenQuad(d,m_fullWidth,m_fullHeight);for(DWORD s=0;s<3;++s)d->SetTexture(s,nullptr);
 }
 {
  ScopedGpuTimer timer(d,GpuPerfStage::AtmosphereComposite);d->SetRenderTarget(0,target);d->SetPixelShader(m_compositeShader.Get());d->SetTexture(0,f.sceneColor);d->SetTexture(1,m_upsampledTexture.Get());d->SetTexture(2,m_boundaryTexture.Get());d->SetTexture(3,f.waterMaskTexture);d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(1,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);d->SetSamplerState(2,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(3,D3DSAMP_MINFILTER,D3DTEXF_POINT);float debug[4]={settings.debugMode==VolumetricDebugMode::None?0.f:1.f,1.f,settings.extinctionStrength,settings.scatterStrength};float source[4]={f.sunScreenX,f.sunScreenY,float(m_fullWidth)/float(m_fullHeight),f.celestialIntensity>0.f?1.f:0.f};float projection[4]={f.projUnpack[0],f.projUnpack[1],aerialEnd,0};d->SetPixelShaderConstantF(0,debug,1);d->SetPixelShaderConstantF(1,source,1);d->SetPixelShaderConstantF(2,projection,1);float edge[4]={settings.edgeFogDistance,settings.edgeFogPower,settings.edgeFogEnabled?1.f:0.f,0};float edgeColor[4]={ambR,ambG,ambB,0};d->SetPixelShaderConstantF(3,edge,1);d->SetPixelShaderConstantF(4,edgeColor,1);float edgeProjection[4]={f.projUnpack[2],f.projUnpack[3],0,0};float worldUp[4]={f.inverseView.m[0][2],f.inverseView.m[1][2],f.inverseView.m[2][2],0};d->SetPixelShaderConstantF(5,edgeProjection,1);d->SetPixelShaderConstantF(6,worldUp,1);DrawScreenQuad(d,m_fullWidth,m_fullHeight);d->SetTexture(0,nullptr);d->SetTexture(1,nullptr);d->SetTexture(2,nullptr);d->SetTexture(3,nullptr);
 }
 m_historyReadIndex=write;m_historyValid=true;m_previousCamera=f.cameraPosition;std::copy(std::begin(f.projUnpack),std::end(f.projUnpack),m_previousProjection);m_previousView=f.viewRaw;m_previousViewValid=f.viewRawValid;m_previousWasMoon=f.celestialIsMoon;m_previousCelestialIntensity=f.celestialIntensity;m_previousLocalLightSignature=LocalLightManager::Instance().SelectionSignature();return true;
}
}
