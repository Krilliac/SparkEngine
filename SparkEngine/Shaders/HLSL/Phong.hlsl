cbuffer PerFrame:register(b1){float4x4 World;float4x4 ViewProj;float3 LightDir;float Padding;float3 LightColor;float SpecPower;}
struct VS_IN{float3 Pos:POSITION;float3 Normal:NORMAL;};
struct PS_IN{float4 Pos:SV_POSITION;float3 WorldN:TEXCOORD0;float3 WorldV:TEXCOORD1;};
PS_IN VS_Phong(VS_IN i){PS_IN o;float4 wp=mul(float4(i.Pos,1),World);o.Pos=mul(wp,ViewProj);o.WorldN=normalize(mul(i.Normal,(float3x3)World));o.WorldV=normalize(-wp.xyz);return o;}
float4 PS_Phong(PS_IN i):SV_Target{
 float NdotL=max(dot(i.WorldN,-LightDir),0);
 float3 diff=NdotL*LightColor;
 float3 H=normalize(-LightDir+i.WorldV);
 float spec=pow(max(dot(i.WorldN,H),0),SpecPower);
 float3 col=diff+spec*LightColor;return float4(col,1);}
