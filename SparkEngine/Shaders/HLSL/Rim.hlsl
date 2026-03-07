cbuffer PerFrame:register(b1){float4x4 WVP;float3 CamPos;float RimPower;}
struct VS_IN{float3 Pos:POSITION;float3 Normal:NORMAL;};
struct PS_IN{float4 Pos:SV_POSITION;float3 Norm:TEXCOORD0;float3 View:TEXCOORD1;};
PS_IN VS_Rim(VS_IN i){PS_IN o;float4 wp=float4(i.Pos,1);o.Pos=mul(wp,WVP);o.Norm=normalize(i.Normal);o.View=normalize(CamPos-i.Pos);return o;}
float4 PS_Rim(PS_IN i):SV_Target{float rim=pow(1-saturate(dot(i.Norm,i.View)),RimPower);return float4(rim,rim,rim,1);}
