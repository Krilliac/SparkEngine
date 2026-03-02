/**
 * PostProcess.glsl - Post-Processing Effects Collection
 * Spark Engine - Cross-platform shader
 *
 * GLSL 460 Core - Fullscreen post-processing effects:
 * - Tone mapping (ACES, Reinhard, Uncharted2)
 * - Gamma correction
 * - Vignette
 * - Chromatic aberration
 * - Film grain
 * - FXAA (Fast Approximate Anti-Aliasing)
 *
 * Select effect via preprocessor defines: TONEMAP, VIGNETTE, CHROMATIC, GRAIN, FXAA
 */
#version 460 core

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(std140, binding = 1) uniform PostProcessConstants
{
    vec2 u_ScreenSize;
    vec2 u_InvScreenSize;
    float u_Exposure;
    float u_Gamma;
    float u_VignetteStrength;
    float u_VignetteRadius;
    float u_ChromaticStrength;
    float u_GrainStrength;
    float u_Time;
    float u_Saturation;
};

layout(binding = 0) uniform sampler2D u_InputTexture;

// ============================================================================
// TONEMAPPING OPERATORS
// ============================================================================

// ACES Filmic Tonemapping
vec3 ACESFilm(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Reinhard Tonemapping
vec3 Reinhard(vec3 color)
{
    return color / (color + vec3(1.0));
}

// Uncharted 2 Tonemapping
vec3 Uncharted2Tonemap(vec3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// ============================================================================
// EFFECTS
// ============================================================================

// Vignette effect
vec3 ApplyVignette(vec3 color, vec2 uv)
{
    vec2 center = uv - 0.5;
    float dist = length(center);
    float vignette = smoothstep(u_VignetteRadius, u_VignetteRadius - u_VignetteStrength, dist);
    return color * vignette;
}

// Chromatic aberration
vec3 ApplyChromaticAberration(vec2 uv)
{
    vec2 dir = uv - 0.5;
    float dist = length(dir);
    vec2 offset = dir * dist * u_ChromaticStrength * u_InvScreenSize;

    float r = texture(u_InputTexture, uv + offset).r;
    float g = texture(u_InputTexture, uv).g;
    float b = texture(u_InputTexture, uv - offset).b;

    return vec3(r, g, b);
}

// Film grain
float Random(vec2 co)
{
    return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);
}

vec3 ApplyGrain(vec3 color, vec2 uv)
{
    float noise = Random(uv + vec2(u_Time)) * 2.0 - 1.0;
    return color + noise * u_GrainStrength;
}

// Saturation adjustment
vec3 ApplySaturation(vec3 color)
{
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    return mix(vec3(luma), color, u_Saturation);
}

// ============================================================================
// FXAA (Simplified)
// ============================================================================

vec3 ApplyFXAA(vec2 uv)
{
    vec3 rgbNW = textureOffset(u_InputTexture, uv, ivec2(-1, -1)).rgb;
    vec3 rgbNE = textureOffset(u_InputTexture, uv, ivec2( 1, -1)).rgb;
    vec3 rgbSW = textureOffset(u_InputTexture, uv, ivec2(-1,  1)).rgb;
    vec3 rgbSE = textureOffset(u_InputTexture, uv, ivec2( 1,  1)).rgb;
    vec3 rgbM  = texture(u_InputTexture, uv).rgb;

    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(0.0312, lumaMax * 0.125))
        return rgbM;

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * 0.25, 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(8.0), max(vec2(-8.0), dir * rcpDirMin)) * u_InvScreenSize;

    vec3 rgbA = 0.5 * (
        texture(u_InputTexture, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(u_InputTexture, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(u_InputTexture, uv + dir * -0.5).rgb +
        texture(u_InputTexture, uv + dir *  0.5).rgb);

    float lumaB = dot(rgbB, luma);

    if (lumaB < lumaMin || lumaB > lumaMax)
        return rgbA;

    return rgbB;
}

// ============================================================================
// MAIN
// ============================================================================
void main()
{
    vec3 color;

#ifdef FXAA_PASS
    color = ApplyFXAA(fragTexCoord);
#elif defined(CHROMATIC_PASS)
    color = ApplyChromaticAberration(fragTexCoord);
#else
    color = texture(u_InputTexture, fragTexCoord).rgb;
#endif

    // Apply exposure
    color *= u_Exposure;

    // Tonemapping
#ifdef TONEMAP_REINHARD
    color = Reinhard(color);
#elif defined(TONEMAP_UNCHARTED2)
    vec3 W = vec3(11.2);
    color = Uncharted2Tonemap(color * 2.0) / Uncharted2Tonemap(W);
#else
    color = ACESFilm(color);
#endif

    // Saturation
    color = ApplySaturation(color);

    // Vignette
#ifdef VIGNETTE_PASS
    color = ApplyVignette(color, fragTexCoord);
#endif

    // Film grain
#ifdef GRAIN_PASS
    color = ApplyGrain(color, fragTexCoord);
#endif

    // Gamma correction
    color = pow(color, vec3(1.0 / u_Gamma));

    outColor = vec4(color, 1.0);
}
