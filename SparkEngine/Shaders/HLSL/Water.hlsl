cbuffer PerFrame:register(b1){float4x4 WorldViewProj;float3 CameraPos;float Time;}
Texture2D RefractionMap:register(t0);
Texture2D ReflectionMap:register(t1);
Texture2D NormalMap:register(t2);
SamplerState SamTr:register(s0);
SamplerState SamRe:register(s1);
struct VS_IN{float3 Pos:POSITION;float2 UV:TEXCOORD0;};
struct PS_IN{float4 Pos:SV_POSITION;float2 UV_Re:TEXCOORD0;float2 UV_Ref:TEXCOORD1;float3 ViewDir:TEXCOORD2;};
PS_IN VS_Water(VS_IN i){PS_IN o;float4 worldPos=float4(i.Pos,1);o.Pos=mul(worldPos,WorldViewProj);
 float2 scroll=i.UV+float2(Time*0.05,Time*0.07);
 float2 dn=NormalMap.Sample(SamTr,scroll).rg*2-1;
 o.UV_Re=i.UV+dn*0.02;o.UV_Ref=i.UV-dn*0.02;
 o.ViewDir=normalize(CameraPos-i.Pos);return o;}
float4 PS_Water(PS_IN i):SV_Target{
 float fresnel=pow(1-saturate(dot(i.ViewDir,float3(0,1,0))),3);
 float4 refr=RefractionMap.Sample(SamTr,i.UV_Re);
 float4 refl=ReflectionMap.Sample(SamRe,i.UV_Ref);
 float4 col=lerp(refr,refl,fresnel);
 col.rgb=lerp(col.rgb,float3(0,0.3,0.5),0.2);
 return float4(col.rgb,1);}
