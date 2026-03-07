cbuffer PerFrame : register(b1)
{
    float4x4 World; float4x4 View; float4x4 Projection;
    float3   CameraPos; float    _padding;
};
Texture2D AlbedoMap       : register(t0);
Texture2D NormalMap       : register(t1);
Texture2D MetalRoughMap   : register(t2);
Texture2D OcclusionMap    : register(t3);
SamplerState SamplerLinear: register(s0);
struct VS_IN { float3 Pos:POSITION; float3 Norm:NORMAL; float2 UV:TEXCOORD0; float3 Tangent:TANGENT; };
struct PS_IN { float4 Pos:SV_POSITION; float3 WorldPos:TEXCOORD0; float3 Normal:NORMAL; float2 UV:TEXCOORD1; float3 Tangent:TEXCOORD2; };
PS_IN VS_Main(VS_IN input)
{
    PS_IN o; float4 worldPos = mul(float4(input.Pos,1), World);
    o.WorldPos = worldPos.xyz;
    o.Pos = mul(mul(worldPos, View), Projection);
    o.Normal = normalize(mul(input.Norm, (float3x3)World));
    o.Tangent = normalize(mul(input.Tangent, (float3x3)World));
    o.UV = input.UV; return o;
}
float3 FresnelSchlick(float cosTheta, float3 F0) { return F0 + (1-F0)*pow(1-cosTheta,5); }
float DistributionGGX(float3 N, float3 H, float rough)
{
    float a2=rough*rough; a2*=a2;
    float NdotH=max(dot(N,H),0);
    float denom=NdotH*NdotH*(a2-1)+1;
    return a2/(PI*denom*denom);
}
float GeometrySchlickGGX(float NdotV,float rough)
{
    float k=(rough+1)*(rough+1)/8;
    return NdotV/(NdotV*(1-k)+k);
}
float GeometrySmith(float3 N,float3 V,float3 L,float rough)
{
    return GeometrySchlickGGX(max(dot(N,V),0),rough)*GeometrySchlickGGX(max(dot(N,L),0),rough);
}
float4 PS_Main(PS_IN i):SV_Target
{
    float3 albedo=pow(AlbedoMap.Sample(SamplerLinear,i.UV).rgb,2.2);
    float3 N=normalize(i.Normal);
    float3 V=normalize(CameraPos-i.WorldPos);
    float metal=MetalRoughMap.Sample(SamplerLinear,i.UV).r;
    float rough=MetalRoughMap.Sample(SamplerLinear,i.UV).g;
    float ao=OcclusionMap.Sample(SamplerLinear,i.UV).r;
    float3 L=normalize(float3(-0.5,-1,-0.3));
    float3 H=normalize(V+L);
    float NDF=DistributionGGX(N,H,rough);
    float G=GeometrySmith(N,V,L,rough);
    float3 F=FresnelSchlick(max(dot(H,V),0),lerp(float3(0.04,0.04,0.04),albedo,metal));
    float3 nom=NDF*G*F;
    float denom=4*max(dot(N,V),0)*max(dot(N,L),0)+0.0001;
    float3 spec=nom/denom;
    float3 kS=F;
    float3 kD=(1-kS)*(1-metal);
    float NdotL=max(dot(N,L),0);
    float3 Lo=(kD*albedo/PI+spec)*NdotL;
    float3 ambient=float3(0.03,0.03,0.03)*albedo*ao;
    float3 color=ambient+Lo;
    color=pow(color,1/2.2);
    return float4(color,1);
}
