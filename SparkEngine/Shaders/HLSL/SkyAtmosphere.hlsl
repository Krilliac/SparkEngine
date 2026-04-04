// =============================================================================
// SkyAtmosphere.hlsl — Physically-based atmospheric scattering
//
// Implements Rayleigh + Mie scattering with sun disc and horizon blend.
// Used by SkyAtmosphereSystem for sky rendering.
// =============================================================================

cbuffer SkyConstants : register(b0)
{
    float4x4 InverseViewProjection;
    float3 SunDirection;
    float SunIntensity;
    float3 SunColor;
    float AtmosphereRadius;
    float3 CameraPosition;
    float PlanetRadius;
    float3 RayleighCoeff; // Rayleigh scattering coefficients (5.5e-6, 13e-6, 22.4e-6)
    float MieCoeff;       // Mie scattering coefficient (~21e-6)
    float MieG;           // Mie phase asymmetry (0.76)
    float2 ScreenSize;
    float Padding;
};

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

// Fullscreen triangle (3 vertices, no vertex buffer needed)
VS_OUTPUT VS_Sky(uint vertexId : SV_VertexID)
{
    VS_OUTPUT o;
    o.TexCoord = float2((vertexId << 1) & 2, vertexId & 2);
    o.Position = float4(o.TexCoord * 2.0 - 1.0, 0.0, 1.0);
    o.Position.y *= -1.0;
    return o;
}

// Rayleigh phase function
float RayleighPhase(float cosTheta)
{
    return (3.0 / (16.0 * 3.14159265)) * (1.0 + cosTheta * cosTheta);
}

// Henyey-Greenstein phase function for Mie scattering
float MiePhase(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 / (4.0 * 3.14159265)) * (1.0 - g2) / (denom * sqrt(denom));
}

// Ray-sphere intersection (returns distance to intersection, -1 if miss)
float RaySphereIntersect(float3 origin, float3 dir, float3 center, float radius)
{
    float3 oc = origin - center;
    float b = dot(oc, dir);
    float c = dot(oc, oc) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    return -b + sqrt(disc);
}

float4 PS_Sky(VS_OUTPUT input) : SV_Target
{
    // Reconstruct world-space ray direction from screen UV
    float2 ndc = input.TexCoord * 2.0 - 1.0;
    ndc.y *= -1.0;
    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 worldPos = mul(clipPos, InverseViewProjection);
    float3 rayDir = normalize(worldPos.xyz / worldPos.w - CameraPosition);

    float3 planetCenter = float3(0.0, -PlanetRadius, 0.0);
    float atmosphereTop = PlanetRadius + AtmosphereRadius;

    // Ray march through atmosphere
    float tMax = RaySphereIntersect(CameraPosition, rayDir, planetCenter, atmosphereTop);
    if (tMax < 0.0) tMax = 1000.0;

    const int NUM_STEPS = 16;
    float stepSize = tMax / float(NUM_STEPS);

    float3 totalRayleigh = float3(0, 0, 0);
    float3 totalMie = float3(0, 0, 0);
    float opticalDepthR = 0.0;
    float opticalDepthM = 0.0;

    float scaleHeightR = AtmosphereRadius * 0.085; // ~8.5km for Earth
    float scaleHeightM = AtmosphereRadius * 0.012; // ~1.2km for Earth

    for (int i = 0; i < NUM_STEPS; i++)
    {
        float t = (float(i) + 0.5) * stepSize;
        float3 samplePos = CameraPosition + rayDir * t;

        float height = length(samplePos - planetCenter) - PlanetRadius;
        height = max(height, 0.0);

        float densityR = exp(-height / scaleHeightR) * stepSize;
        float densityM = exp(-height / scaleHeightM) * stepSize;

        opticalDepthR += densityR;
        opticalDepthM += densityM;

        // Light optical depth (simplified — single sample toward sun)
        float lightHeight = height + SunDirection.y * stepSize * 2.0;
        lightHeight = max(lightHeight, 0.0);
        float lightOptR = exp(-lightHeight / scaleHeightR) * stepSize * 2.0;
        float lightOptM = exp(-lightHeight / scaleHeightM) * stepSize * 2.0;

        float3 attenuation = exp(-(RayleighCoeff * (opticalDepthR + lightOptR) +
                                   MieCoeff * (opticalDepthM + lightOptM)));

        totalRayleigh += densityR * attenuation;
        totalMie += densityM * attenuation;
    }

    float cosTheta = dot(rayDir, SunDirection);
    float phaseR = RayleighPhase(cosTheta);
    float phaseM = MiePhase(cosTheta, MieG);

    float3 sky = SunIntensity * SunColor *
                 (phaseR * RayleighCoeff * totalRayleigh + phaseM * MieCoeff * totalMie);

    // Sun disc
    float sunAngle = acos(saturate(cosTheta));
    float sunRadius = 0.0087; // ~0.5 degrees angular radius
    float sunMask = smoothstep(sunRadius, sunRadius * 0.8, sunAngle);
    sky += SunColor * SunIntensity * sunMask * 100.0;

    // Horizon blend
    float horizonFade = saturate(rayDir.y * 3.0 + 0.2);
    sky = lerp(sky * 0.3, sky, horizonFade);

    return float4(sky, 1.0);
}
