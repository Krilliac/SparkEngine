/**
 * @file GTAOEffect.h
 * @brief Ground Truth Ambient Occlusion (GTAO) screen-space effect
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements GTAO (Jimenez et al. 2016, "Practical Realtime Strategies for
 * Accurate Indirect Occlusion"): for each pixel, marches in multiple slice
 * directions along the depth buffer to find the two horizon angles, measured
 * from the view vector, then integrates the cosine-weighted visibility of the
 * arc between them analytically, using the normal projected into the slice.
 * An unoccluded plane integrates to exactly 1. Includes spatial denoising via
 * a 3x3 cross-bilateral filter that preserves depth edges.
 *
 * Features:
 * - Configurable number of directions (4-8) and steps per direction (4-8)
 * - Horizon-based AO with cosine weighting
 * - Spatial denoising with depth-aware bilateral filter
 * - HLSL compute shader string for GPU implementation
 * - CPU reference path for validation
 *
 * @see SSAOEffect.h, PostProcessing.h, RenderGraph.h
 *
 * @note **Lifecycle-only activation (Phase I)** — Settings are wired into PostProcessingPipeline
 * but the GPU compute path (ComputeGTAO) is not called from any render pass. The CPU reference
 * path is exercised by tests. Full GPU activation requires a compute-shader dispatch step in the
 * post-process pipeline, which depends on the HLSL compute shader being compiled and bound.
 *
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    static constexpr float GTAO_PI = 3.14159265358979323846f;
    static constexpr float GTAO_HALF_PI = 0.5f * GTAO_PI;

    // =========================================================================
    // GTAO Settings
    // =========================================================================

    struct GTAOSettings
    {
        int directions = 6;          ///< Number of horizon search directions [4-8]
        int stepsPerDirection = 6;   ///< Steps along each direction [4-8]
        float radius = 1.5f;         ///< World-space AO radius
        float power = 1.5f;          ///< AO intensity power curve
        float falloffStart = 0.2f;   ///< Start of distance falloff (fraction of radius)
        float falloffEnd = 1.0f;     ///< End of distance falloff
        float depthThreshold = 0.1f; ///< Bilateral filter depth edge threshold
        float normalBias = 0.01f;    ///< Small offset to prevent self-occlusion
    };

    // =========================================================================
    // GTAO Effect
    // =========================================================================

    /**
     * @brief CPU reference implementation of Ground Truth Ambient Occlusion
     *
     * On GPU, ComputeGTAO maps to a full-screen compute dispatch and
     * SpatialDenoise to a second pass. The CPU path is for validation.
     */
    class GTAOEffect
    {
      public:
        GTAOEffect() = default;
        ~GTAOEffect() = default;

        /// @brief Initialize with screen dimensions
        bool Initialize(uint32_t width, uint32_t height, const GTAOSettings& settings)
        {
            if (width == 0 || height == 0 || width > 16384 || height > 16384)
                return false;
            m_width = width;
            m_height = height;
            m_settings = settings;
            m_aoBuffer.resize(static_cast<size_t>(width) * height, 1.0f);
            m_denoisedBuffer.resize(static_cast<size_t>(width) * height, 1.0f);
            m_initialized = true;
            return true;
        }

        /// @brief Shutdown and release buffers
        void Shutdown()
        {
            m_aoBuffer.clear();
            m_denoisedBuffer.clear();
            m_initialized = false;
        }

        /**
         * @brief Compute GTAO for all pixels
         *
         * For each pixel, marches in m_settings.directions evenly-spaced
         * directions. Along each direction, samples the depth buffer at
         * m_settings.stepsPerDirection intervals on both sides to find the
         * highest horizon, and integrates the slice visibility with
         * SliceVisibility(). AO = pow(mean slice visibility, power).
         *
         * @param depthBuffer   Linearized depth buffer (width * height)
         * @param normalBuffer  View-space normals (width * height * 3, interleaved XYZ);
         *                      +Z faces the camera, X/Y follow the buffer's +x/+y axes
         * @param projScale     Projection scale factor (focal_length / viewport_height)
         */
        void ComputeGTAO(const float* depthBuffer, const float* normalBuffer, float projScale)
        {
            if (!m_initialized || !depthBuffer || !normalBuffer)
                return;

            for (uint32_t y = 0; y < m_height; ++y)
            {
                for (uint32_t x = 0; x < m_width; ++x)
                {
                    uint32_t pixelIdx = y * m_width + x;
                    float centerDepth = depthBuffer[pixelIdx];

                    if (centerDepth <= 0.0f)
                    {
                        m_aoBuffer[pixelIdx] = 1.0f;
                        continue;
                    }

                    // Surface normal at this pixel
                    float nx = normalBuffer[pixelIdx * 3 + 0];
                    float ny = normalBuffer[pixelIdx * 3 + 1];
                    float nz = normalBuffer[pixelIdx * 3 + 2];

                    // Screen-space radius from world-space radius
                    float screenRadius = (m_settings.radius * projScale) / centerDepth;
                    screenRadius = std::max(1.0f, std::min(screenRadius, 256.0f));

                    float totalAO = 0.0f;
                    int dirs = m_settings.directions;
                    int steps = m_settings.stepsPerDirection;

                    // Horizon vectors use +Z toward the camera, like the normal buffer,
                    // so the view vector (surface to camera) is +Z.
                    const float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);

                    // March in evenly-spaced directions around the pixel
                    for (int d = 0; d < dirs; ++d)
                    {
                        float angle = GTAO_PI * d / static_cast<float>(dirs);
                        float dirX = std::cos(angle);
                        float dirY = std::sin(angle);

                        // Project the normal into the slice spanned by (dirX, dirY, 0) and
                        // the view vector (0, 0, 1); n is its signed angle from the view vector.
                        const float nAlongDir = nLen > 0.0f ? (nx * dirX + ny * dirY) / nLen : 0.0f;
                        const float nAlongView = nLen > 0.0f ? nz / nLen : 1.0f;
                        const float projLen = std::sqrt(nAlongDir * nAlongDir + nAlongView * nAlongView);
                        const float cosN = std::clamp(nAlongView / std::max(projLen, 0.0001f), 0.0f, 1.0f);
                        const float n = (nAlongDir < 0.0f ? -1.0f : 1.0f) * std::acos(cosN);

                        // Horizon cosines (from the view vector) start at the tangent plane:
                        // unoccluded. Faded samples blend back toward it.
                        const float lowCos = std::cos(n + GTAO_HALF_PI);
                        const float lowCosNeg = std::cos(n - GTAO_HALF_PI);

                        // Find maximum horizon angle in this direction
                        float maxHorizonCos = lowCos;

                        for (int s = 1; s <= steps; ++s)
                        {
                            float stepFraction = static_cast<float>(s) / static_cast<float>(steps);
                            float offset = stepFraction * screenRadius;

                            int sampleX = static_cast<int>(x + dirX * offset + 0.5f);
                            int sampleY = static_cast<int>(y + dirY * offset + 0.5f);

                            // Bounds check
                            if (sampleX < 0 || sampleX >= static_cast<int>(m_width) || sampleY < 0 ||
                                sampleY >= static_cast<int>(m_height))
                            {
                                continue;
                            }

                            uint32_t sampleIdx = sampleY * m_width + sampleX;
                            float sampleDepth = depthBuffer[sampleIdx];

                            if (sampleDepth <= 0.0f)
                                continue;

                            // Compute horizon vector in view space (+Z toward the camera)
                            float deltaX = (sampleX - static_cast<float>(x)) / projScale * centerDepth;
                            float deltaY = (sampleY - static_cast<float>(y)) / projScale * centerDepth;
                            float deltaZ = centerDepth - sampleDepth;

                            float horizLen = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
                            if (horizLen < 0.0001f)
                                continue;

                            // Horizon angle cosine relative to the view vector
                            float horizCos = deltaZ / horizLen;

                            // Distance falloff
                            float dist = horizLen;
                            float falloff = 1.0f;
                            float falloffStart = m_settings.falloffStart * m_settings.radius;
                            float falloffEnd = m_settings.falloffEnd * m_settings.radius;
                            if (dist > falloffStart)
                            {
                                falloff =
                                    1.0f - std::clamp((dist - falloffStart) / (falloffEnd - falloffStart + 0.001f),
                                                      0.0f, 1.0f);
                            }

                            float weightedCos = lowCos + (horizCos - lowCos) * falloff;
                            maxHorizonCos = std::max(maxHorizonCos, weightedCos);
                        }

                        // Also search negative direction
                        float maxHorizonCosNeg = lowCosNeg;

                        for (int s = 1; s <= steps; ++s)
                        {
                            float stepFraction = static_cast<float>(s) / static_cast<float>(steps);
                            float offset = stepFraction * screenRadius;

                            int sampleX = static_cast<int>(x - dirX * offset + 0.5f);
                            int sampleY = static_cast<int>(y - dirY * offset + 0.5f);

                            if (sampleX < 0 || sampleX >= static_cast<int>(m_width) || sampleY < 0 ||
                                sampleY >= static_cast<int>(m_height))
                            {
                                continue;
                            }

                            uint32_t sampleIdx = sampleY * m_width + sampleX;
                            float sampleDepth = depthBuffer[sampleIdx];

                            if (sampleDepth <= 0.0f)
                                continue;

                            float deltaX = (sampleX - static_cast<float>(x)) / projScale * centerDepth;
                            float deltaY = (sampleY - static_cast<float>(y)) / projScale * centerDepth;
                            float deltaZ = centerDepth - sampleDepth;

                            float horizLen = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
                            if (horizLen < 0.0001f)
                                continue;

                            float horizCos = deltaZ / horizLen;

                            float dist = horizLen;
                            float falloff = 1.0f;
                            float falloffStart = m_settings.falloffStart * m_settings.radius;
                            float falloffEnd = m_settings.falloffEnd * m_settings.radius;
                            if (dist > falloffStart)
                            {
                                falloff =
                                    1.0f - std::clamp((dist - falloffStart) / (falloffEnd - falloffStart + 0.001f),
                                                      0.0f, 1.0f);
                            }

                            float weightedCos = lowCosNeg + (horizCos - lowCosNeg) * falloff;
                            maxHorizonCosNeg = std::max(maxHorizonCosNeg, weightedCos);
                        }

                        // Signed horizon angles (the negative direction is negative)
                        float h1 = std::acos(std::clamp(maxHorizonCos, -1.0f, 1.0f));
                        float h0 = -std::acos(std::clamp(maxHorizonCosNeg, -1.0f, 1.0f));

                        totalAO += SliceVisibility(n, projLen, h0, h1);
                    }

                    float ao = totalAO / static_cast<float>(dirs);
                    ao = std::pow(std::clamp(ao, 0.0f, 1.0f), m_settings.power);
                    m_aoBuffer[pixelIdx] = ao;
                }
            }
        }

        /**
         * @brief 3x3 cross-bilateral spatial denoiser
         *
         * Blurs the AO buffer while preserving depth edges. Samples in a
         * cross pattern (not full 3x3) weighted by depth similarity.
         *
         * @param depthBuffer  Linearized depth buffer for edge detection
         */
        void SpatialDenoise(const float* depthBuffer)
        {
            if (!m_initialized || !depthBuffer)
                return;

            // 3x3 cross-bilateral offsets
            static constexpr int offsets[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};

            for (uint32_t y = 0; y < m_height; ++y)
            {
                for (uint32_t x = 0; x < m_width; ++x)
                {
                    uint32_t centerIdx = y * m_width + x;
                    float centerDepth = depthBuffer[centerIdx];
                    float centerAO = m_aoBuffer[centerIdx];

                    float weightedSum = centerAO;
                    float totalWeight = 1.0f;

                    for (const auto& off : offsets)
                    {
                        int sx = static_cast<int>(x) + off[0];
                        int sy = static_cast<int>(y) + off[1];

                        if (sx < 0 || sx >= static_cast<int>(m_width) || sy < 0 || sy >= static_cast<int>(m_height))
                        {
                            continue;
                        }

                        uint32_t sampleIdx = sy * m_width + sx;
                        float sampleDepth = depthBuffer[sampleIdx];
                        float sampleAO = m_aoBuffer[sampleIdx];

                        // Depth-based bilateral weight
                        float depthDiff = std::abs(centerDepth - sampleDepth);
                        float weight = std::exp(-depthDiff / (m_settings.depthThreshold + 0.001f));

                        weightedSum += sampleAO * weight;
                        totalWeight += weight;
                    }

                    m_denoisedBuffer[centerIdx] = weightedSum / totalWeight;
                }
            }
        }

        /**
         * @brief Cosine-weighted visibility of one GTAO slice
         *
         * Angles are measured from the view vector inside the slice. The horizons
         * are clamped to the normal's hemisphere [n - pi/2, n + pi/2], then
         * a(h) = (-cos(2h - n) + cos(n) + 2 h sin(n)) / 4 is integrated over both
         * sides and scaled by the projected normal length. An open plane facing
         * the viewer (n = 0, projLen = 1, h0 = -pi/2, h1 = pi/2) gives exactly 1.
         *
         * @param n        Signed angle of the projected normal from the view vector
         * @param projLen  Length of the normal projected into the slice plane
         * @param h0       Horizon angle on the negative side (<= 0 before clamping)
         * @param h1       Horizon angle on the positive side (>= 0 before clamping)
         */
        static float SliceVisibility(float n, float projLen, float h0, float h1)
        {
            h0 = n + std::clamp(h0 - n, -GTAO_HALF_PI, GTAO_HALF_PI);
            h1 = n + std::clamp(h1 - n, -GTAO_HALF_PI, GTAO_HALF_PI);
            const float cosN = std::cos(n);
            const float sinN = std::sin(n);
            auto arc = [&](float h) { return 0.25f * (-std::cos(2.0f * h - n) + cosN + 2.0f * h * sinN); };
            return std::max(projLen * (arc(h0) + arc(h1)), 0.0f);
        }

        /// @brief Get the raw (non-denoised) AO buffer
        const std::vector<float>& GetAOBuffer() const { return m_aoBuffer; }

        /// @brief Get the denoised AO buffer
        const std::vector<float>& GetDenoisedBuffer() const { return m_denoisedBuffer; }

        /// @brief Get current settings
        const GTAOSettings& GetSettings() const { return m_settings; }

        /// @brief Check if initialized
        bool IsInitialized() const { return m_initialized; }

        /**
         * @brief HLSL compute shader source for GPU GTAO
         *
         * This shader implements the same algorithm as the CPU path.
         * Bind depth as SRV t0, normals as SRV t1, output as UAV u0.
         */
        static constexpr const char* GetHLSLShaderSource()
        {
            return R"(
// GTAO Compute Shader
// Dispatch: ceil(width/8) x ceil(height/8) x 1

cbuffer GTAOConstants : register(b0)
{
    float4 ScreenParams;   // width, height, 1/width, 1/height
    float  Radius;
    float  Power;
    float  ProjScale;
    int    Directions;
    int    StepsPerDir;
    float  FalloffStart;
    float  FalloffEnd;
    float  Padding;
};

Texture2D<float>  DepthTex   : register(t0);
Texture2D<float3> NormalTex  : register(t1);
RWTexture2D<float> AOOutput  : register(u0);

static const float PI = 3.14159265;
static const float HALF_PI = 1.57079633;

// Cosine-weighted arc integral of one slice side (Jimenez et al. 2016); angles
// from the view vector, n = projected normal angle. Same as SliceVisibility().
float SliceArc(float h, float n)
{
    return 0.25 * (-cos(2.0 * h - n) + cos(n) + 2.0 * h * sin(n));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= (uint)ScreenParams.x || DTid.y >= (uint)ScreenParams.y)
        return;

    float2 uv = (DTid.xy + 0.5) * ScreenParams.zw;
    float  depth = DepthTex[DTid.xy];
    float3 normal = NormalTex[DTid.xy] * 2.0 - 1.0;

    if (depth <= 0.0)
    {
        AOOutput[DTid.xy] = 1.0;
        return;
    }

    float screenRadius = max(1.0, min(Radius * ProjScale / depth, 256.0));
    float totalAO = 0.0;
    // +Z faces the camera (as in the normal texture), so the view vector is +Z.
    normal = normalize(normal);

    for (int d = 0; d < Directions; d++)
    {
        float angle = PI * d / (float)Directions;
        float2 dir = float2(cos(angle), sin(angle));

        float nAlongDir = dot(normal.xy, dir);
        float projLen = length(float2(nAlongDir, normal.z));
        float cosN = saturate(normal.z / max(projLen, 0.0001));
        float n = (nAlongDir < 0.0 ? -1.0 : 1.0) * acos(cosN);
        float lowCos = cos(n + HALF_PI);
        float lowCosNeg = cos(n - HALF_PI);

        float maxH = lowCos;
        for (int s = 1; s <= StepsPerDir; s++)
        {
            float2 samplePos = DTid.xy + dir * (s / (float)StepsPerDir * screenRadius);
            int2 sp = (int2)samplePos;
            if (sp.x < 0 || sp.x >= (int)ScreenParams.x ||
                sp.y < 0 || sp.y >= (int)ScreenParams.y) continue;

            float sd = DepthTex[sp];
            if (sd <= 0.0) continue;

            float3 delta = float3((sp - (int2)DTid.xy) / ProjScale * depth, depth - sd);
            float len = length(delta);
            if (len < 0.0001) continue;

            float hc = delta.z / len;
            float falloff = 1.0 - saturate((len - FalloffStart * Radius) /
                                            (FalloffEnd * Radius - FalloffStart * Radius + 0.001));
            maxH = max(maxH, lerp(lowCos, hc, falloff));
        }

        float maxHNeg = lowCosNeg;
        for (int s = 1; s <= StepsPerDir; s++)
        {
            float2 samplePos = DTid.xy - dir * (s / (float)StepsPerDir * screenRadius);
            int2 sp = (int2)samplePos;
            if (sp.x < 0 || sp.x >= (int)ScreenParams.x ||
                sp.y < 0 || sp.y >= (int)ScreenParams.y) continue;

            float sd = DepthTex[sp];
            if (sd <= 0.0) continue;

            float3 delta = float3((sp - (int2)DTid.xy) / ProjScale * depth, depth - sd);
            float len = length(delta);
            if (len < 0.0001) continue;

            float hc = delta.z / len;
            float falloff = 1.0 - saturate((len - FalloffStart * Radius) /
                                            (FalloffEnd * Radius - FalloffStart * Radius + 0.001));
            maxHNeg = max(maxHNeg, lerp(lowCosNeg, hc, falloff));
        }

        float h0 = n + clamp(-acos(clamp(maxHNeg, -1.0, 1.0)) - n, -HALF_PI, HALF_PI);
        float h1 = n + clamp(acos(clamp(maxH, -1.0, 1.0)) - n, -HALF_PI, HALF_PI);
        totalAO += max(projLen * (SliceArc(h0, n) + SliceArc(h1, n)), 0.0);
    }

    float ao = pow(saturate(totalAO / (float)Directions), Power);
    AOOutput[DTid.xy] = ao;
}
)";
        }

      private:
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        GTAOSettings m_settings;
        std::vector<float> m_aoBuffer;
        std::vector<float> m_denoisedBuffer;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
