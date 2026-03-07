Texture2D InputTex:register(t0);SamplerState Sam:register(s0);
cbuffer ThresholdCB:register(b1){float Threshold;}
float4 PS_BloomExtract(float2 uv:TEXCOORD):SV_Target
{
    float3 c=InputTex.Sample(Sam,uv).rgb;
    float lum=dot(c,float3(0.299,0.587,0.114));
    return float4((lum>Threshold)?c:0,1);
}
