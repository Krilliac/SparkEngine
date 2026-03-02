/**
 * Utils.glsl - Shared Utility Functions
 * Spark Engine - Cross-platform shader (GLSL equivalent of HLSL Utils)
 *
 * GLSL 460 Core - Common utility functions shared across shaders.
 * Include via #include "Utils.glsl" or copy needed functions.
 */

// ============================================================================
// CONSTANTS
// ============================================================================
const float PI         = 3.14159265359;
const float TWO_PI     = 6.28318530718;
const float HALF_PI    = 1.57079632679;
const float INV_PI     = 0.31830988618;
const float INV_TWO_PI = 0.15915494309;
const float EPSILON    = 0.0001;

// ============================================================================
// COLOR SPACE CONVERSIONS
// ============================================================================

// Linear to sRGB
vec3 LinearToSRGB(vec3 color)
{
    return pow(color, vec3(1.0 / 2.2));
}

// sRGB to Linear
vec3 SRGBToLinear(vec3 color)
{
    return pow(color, vec3(2.2));
}

// RGB to luminance
float Luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// RGB to HSV
vec3 RGBToHSV(vec3 c)
{
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// HSV to RGB
vec3 HSVToRGB(vec3 c)
{
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// ============================================================================
// MATH UTILITIES
// ============================================================================

// Remap value from one range to another
float Remap(float value, float inMin, float inMax, float outMin, float outMax)
{
    return outMin + (value - inMin) * (outMax - outMin) / (inMax - inMin);
}

// Smooth minimum (for smooth blending)
float SmoothMin(float a, float b, float k)
{
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// Inverse lerp
float InverseLerp(float a, float b, float v)
{
    return clamp((v - a) / (b - a), 0.0, 1.0);
}

// ============================================================================
// NOISE FUNCTIONS
// ============================================================================

// Simple hash for noise generation
float Hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Value noise
float ValueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); // Hermite interpolation

    float a = Hash(i);
    float b = Hash(i + vec2(1.0, 0.0));
    float c = Hash(i + vec2(0.0, 1.0));
    float d = Hash(i + vec2(1.0, 1.0));

    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Fractal Brownian Motion
float FBM(vec2 p, int octaves)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for (int i = 0; i < octaves; i++)
    {
        value += amplitude * ValueNoise(p * frequency);
        frequency *= 2.0;
        amplitude *= 0.5;
    }

    return value;
}

// ============================================================================
// TEXTURE UTILITIES
// ============================================================================

// Parallax occlusion mapping offset
vec2 ParallaxOffset(float height, float scale, vec3 viewDir)
{
    float h = height * scale - scale * 0.5;
    return viewDir.xy / viewDir.z * h;
}

// Normal map decode (standard tangent-space)
vec3 UnpackNormal(vec2 encoded)
{
    vec3 n;
    n.xy = encoded * 2.0 - 1.0;
    n.z = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

// Encode normal for G-buffer storage
vec2 PackNormal(vec3 n)
{
    return n.xy * 0.5 + 0.5;
}

// ============================================================================
// SHADOW UTILITIES
// ============================================================================

// PCF shadow sampling (4-tap)
float ShadowPCF4(sampler2D shadowMap, vec3 shadowCoord, float bias)
{
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));

    for (int x = -1; x <= 1; x += 2)
    {
        for (int y = -1; y <= 1; y += 2)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            float depth = texture(shadowMap, shadowCoord.xy + offset).r;
            shadow += (shadowCoord.z - bias > depth) ? 0.0 : 1.0;
        }
    }

    return shadow / 4.0;
}
