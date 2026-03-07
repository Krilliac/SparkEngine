Texture2D NormalTex:register(t0);
Texture2D DepthTex:register(t1);
SamplerState Sam:register(s0);
struct PS_IN{float2 uv:TEXCOORD;};
float4 PS_DebugNorm(PS_IN i):SV_Target{return float4(NormalTex.Sample(Sam,i.uv).xyz*0.5+0.5,1);}
float4 PS_DebugDepth(PS_IN i):SV_Target{float d=DepthTex.Sample(Sam,i.uv).r;return float4(d,d,d,1);}
