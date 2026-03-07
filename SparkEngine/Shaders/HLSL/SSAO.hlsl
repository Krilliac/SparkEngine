cbuffer CB:register(b1){float4x4 InvProj;float2 ScreenSize;float Radius;float Bias;}
Texture2D DepthTex:register(t0);SamplerState Sam:register(s0);
static const float2 samples[16]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1),float2(.707,.707),float2(-.707,.707),float2(.707,-.707),float2(-.707,-.707),float2(1,1),float2(-1,1),float2(1,-1),float2(-1,-1),float2(.5,0),float2(-.5,0),float2(0,.5),float2(0,-.5)};
float4 main(float2 uv:TEXCOORD):SV_Target{
 float z=DepthTex.Sample(Sam,uv).r;
 float3 pos=mul(float4(uv*2-1,z,1),InvProj).xyz;
 float oc=0;
 for(int i=0;i<16;i++){
  float2 off=samples[i]/ScreenSize*Radius;
  float z2=DepthTex.Sample(Sam,uv+off).r;
  float3 pos2=mul(float4((uv+off)*2-1,z2,1),InvProj).xyz;
  oc+=((pos.z-pos2.z)>Bias)?1:0;
 }
 oc/=16;
 return float4(1-oc,1-oc,1-oc,1);}
