/**
 * @file RTComposite.hlsl
 * @brief Composites screen-space and ray-traced results with temporal accumulation
 *
 * CS 5.0 compatible. Blends SSR/SSAO with SDFGI/DXR results based on:
 * - Roughness (smooth surfaces prefer RT reflections, rough use probes)
 * - Confidence (RT miss → use screen-space, RT hit → use RT result)
 * - Temporal stability (exponential moving average with motion rejection)
 *
 * Dispatch: ceil(width/8) x ceil(height/8) x 1
 */

// ============================================================================
// Resources
// ============================================================================

Texture2D<float4> g_RTReflections : register(t0);  ///< Ray-traced reflections (SDFGI or DXR)
Texture2D<float4> g_RTGI : register(t1);           ///< Ray-traced global illumination
Texture2D<float4> g_RTShadows : register(t2);      ///< Ray-traced soft shadows
Texture2D<float4> g_SSReflections : register(t3);   ///< Screen-space reflections (existing SSR)
Texture2D<float4> g_SSAO : register(t4);            ///< Screen-space ambient occlusion
Texture2D<float4> g_LightingBuffer : register(t5); ///< Current lighting result
Texture2D<float4> g_GBufferNormals : register(t6);
Texture2D<float4> g_GBufferAlbedo : register(t7);
Texture2D<float4> g_History : register(t8);         ///< Previous frame composite

RWTexture2D<float4> g_Output : register(u0);

cbuffer CompositeConstants : register(b0)
{
    float ScreenWidth;
    float ScreenHeight;
    float TemporalBlend;      ///< 0.9 = 90% history, 10% current
    float ReflectionStrength;
    float GIStrength;
    float ShadowStrength;
    float SSRFallbackWeight;  ///< How much to use SSR when RT misses
    float FrameIndex;         ///< For temporal jitter
};

// ============================================================================
// Compositing Logic
// ============================================================================

[numthreads(8, 8, 1)]
void CSComposite(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= (uint)ScreenWidth || dtid.y >= (uint)ScreenHeight)
        return;

    float4 lighting = g_LightingBuffer[dtid.xy];
    float4 rtRefl = g_RTReflections[dtid.xy];
    float4 rtGI = g_RTGI[dtid.xy];
    float4 rtShadow = g_RTShadows[dtid.xy];
    float4 ssRefl = g_SSReflections[dtid.xy];
    float4 ssao = g_SSAO[dtid.xy];
    float4 normalData = g_GBufferNormals[dtid.xy];

    // Extract roughness from GBuffer alpha (convention: normal.a = roughness)
    float roughness = normalData.a;

    // --- Reflections: blend RT and SS based on confidence and roughness ---
    float rtConfidence = rtRefl.a; // Alpha encodes hit confidence
    float ssWeight = SSRFallbackWeight * (1.0 - rtConfidence);
    float rtWeight = rtConfidence * ReflectionStrength;

    // Rough surfaces use less specular reflection
    float roughnessFade = 1.0 - smoothstep(0.3, 0.8, roughness);
    float3 reflections = lerp(ssRefl.rgb, rtRefl.rgb, rtConfidence) * roughnessFade;

    // --- Global illumination: additive to lighting ---
    float3 gi = rtGI.rgb * GIStrength * rtGI.a; // Alpha = GI confidence

    // --- Shadows: multiply with existing lighting ---
    float shadowFactor = lerp(1.0, rtShadow.r, ShadowStrength);

    // --- AO: combine SSAO with RT AO (RT shadows contain AO information) ---
    float ao = min(ssao.r, shadowFactor);

    // --- Composite ---
    float3 result = lighting.rgb;
    result *= ao;          // Apply ambient occlusion
    result *= shadowFactor; // Apply ray-traced shadows
    result += gi;           // Add indirect illumination
    result += reflections * ReflectionStrength; // Add reflections

    // --- Temporal accumulation ---
    float4 history = g_History[dtid.xy];
    float3 temporalResult = lerp(result, history.rgb, TemporalBlend);

    // Simple luminance-based rejection: if current differs too much, reduce history weight
    float lumCurrent = dot(result, float3(0.2126, 0.7152, 0.0722));
    float lumHistory = dot(history.rgb, float3(0.2126, 0.7152, 0.0722));
    float lumDiff = abs(lumCurrent - lumHistory) / max(lumCurrent, 0.001);
    if (lumDiff > 0.5)
    {
        temporalResult = lerp(result, history.rgb, TemporalBlend * 0.3); // Reduce history on disocclusion
    }

    g_Output[dtid.xy] = float4(temporalResult, 1.0);
}
