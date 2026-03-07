Texture2D InputTex:register(t0);SamplerState Sam:register(s0);
cbuffer BlurCB:register(b1){float2 TexelSize;}
static const float weights[5]={0.204164,0.304005,0.193783,0.0727,0.0155};
float4 PS_BlurH(float2 uv:TEXCOORD):SV_Target{float4 c=InputTex.Sample(Sam,uv)*weights[0];
    for(int i=1;i<5;i++){c+=InputTex.Sample(Sam,uv+float2(TexelSize.x*i,0))*weights[i];c+=InputTex.Sample(Sam,uv-float2(TexelSize.x*i,0))*weights[i];}
    return c;
}
float4 PS_BlurV(float2 uv:TEXCOORD):SV_Target{float4 c=InputTex.Sample(Sam,uv)*weights[0];
    for(int i=1;i<5;i++){c+=InputTex.Sample(Sam,uv+float2(0,TexelSize.y*i))*weights[i];c+=InputTex.Sample(Sam,uv-float2(0,TexelSize.y*i))*weights[i];}
    return c;
}
