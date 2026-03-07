cbuffer PerFrame : register(b1)
{
    float4x4 WVProj; float3 CameraPos; float Roughness;
};
TextureCube EnvMap   : register(t0);
SamplerState EnvSamp : register(s0);
struct VS_OUT { float4 Pos:SV_POSITION; float3 WorldPos:TEXCOORD0; float3 Normal:TEXCOORD1; };
VS_OUT VS_Env(float3 pos:POSITION,float3 norm:NORMAL)
{
    VS_OUT o; o.WorldPos=pos;
    o.Pos=mul(float4(pos,1),WVProj);
    o.Normal=normalize(norm);
    return o;
}
float4 PS_Env(VS_OUT i):SV_Target
{
    float3 R=reflect(normalize(i.WorldPos-CameraPos),normalize(i.Normal));
    float3 col=EnvMap.SampleLevel(EnvSamp,R,Roughness*64);
    return float4(col,1);
}
