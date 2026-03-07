struct PS_INPUT
{
    float4 Pos      : SV_POSITION;
    float3 WorldPos : POSITION1;
    float3 Normal   : NORMAL;
    float2 Tex      : TEXCOORD0;
};

cbuffer LightBuffer : register(b1)
{
    float3 LightDirection;
    float  LightIntensity;
    float3 LightColor;
    float  AmbientIntensity;
    float3 CameraPosition;
    float  SpecularPower;
};

float4 main(PS_INPUT input) : SV_Target
{
    float3 normal    = normalize(input.Normal);
    float3 lightDir  = normalize(-LightDirection);
    float3 ambient   = LightColor * AmbientIntensity;
    float  diff      = max(dot(normal, lightDir), 0.0f);
    float3 diffuse   = LightColor * LightIntensity * diff;
    float3 viewDir   = normalize(CameraPosition - input.WorldPos);
    float3 refl      = reflect(-lightDir, normal);
    float  spec      = pow(max(dot(viewDir, refl), 0.0f), SpecularPower);
    float3 specular  = LightColor * spec;
    float3 finalCol  = ambient + diffuse + specular;
    float3 matColor  = float3(0.7f, 0.7f, 0.7f);
    return float4(finalCol * matColor, 1.0f);
}
