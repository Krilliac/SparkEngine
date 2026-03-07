Texture2D DebugTex:register(t0);SamplerState Sam:register(s0);
struct PS_IN{float2 uv:TEXCOORD;};
float4 PS_Checker(PS_IN i):SV_Target{float s=step(0.5,frac(i.uv.x*10))^step(0.5,frac(i.uv.y*10));return float4(s,s,s,1);}
float4 PS_Gradient(PS_IN i):SV_Target{return float4(i.uv.x,i.uv.y,0,1);}
