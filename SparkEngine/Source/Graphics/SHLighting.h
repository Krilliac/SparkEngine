/**
 * @file SHLighting.h
 * @brief Spherical harmonics evaluation and BRDF LUT generation for IBL (Filament-inspired)
 *
 * Provides CPU-side SH evaluation functions for image-based lighting:
 * - Project directional irradiance into L2 SH coefficients
 * - Evaluate SH irradiance at a given normal direction
 * - Generate a BRDF integration LUT for split-sum PBR approximation
 *
 * The SH representation uses 9 coefficients (L0 + L1 + L2) per color channel,
 * matching Google Filament's approach: <1% error at near-zero runtime cost
 * compared to full cubemap convolution.
 *
 * @see LightProbeSystem for probe placement and interpolation
 * @see LightingSystem for environment map management
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief SH basis function evaluation and irradiance reconstruction.
     *
     * Uses real-valued spherical harmonics up to L=2 (9 coefficients).
     * Convention: coefficients store irradiance (not radiance), pre-multiplied
     * by the cosine-lobe convolution constants (Ramamoorthi & Hanrahan 2001).
     */
    class SHLighting
    {
      public:
        static constexpr int SH_COEFFICIENT_COUNT = 9;
        static constexpr float PI = 3.14159265358979323846f;

        /// @brief RGB SH coefficient set (9 coefficients, 3 channels each)
        struct SHCoefficients
        {
            std::array<float, SH_COEFFICIENT_COUNT> r{};
            std::array<float, SH_COEFFICIENT_COUNT> g{};
            std::array<float, SH_COEFFICIENT_COUNT> b{};

            void Scale(float factor)
            {
                for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
                {
                    r[i] *= factor;
                    g[i] *= factor;
                    b[i] *= factor;
                }
            }

            void Accumulate(const SHCoefficients& other, float weight)
            {
                for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
                {
                    r[i] += other.r[i] * weight;
                    g[i] += other.g[i] * weight;
                    b[i] += other.b[i] * weight;
                }
            }
        };

        /**
         * @brief Evaluate the 9 SH basis functions for a direction.
         *
         * @param nx, ny, nz  Normalized direction vector
         * @param outBasis    Output array of 9 basis values
         */
        static void EvaluateBasis(float nx, float ny, float nz, float outBasis[SH_COEFFICIENT_COUNT])
        {
            // L0
            outBasis[0] = 0.282095f; // Y_0^0 = 1/(2*sqrt(pi))

            // L1
            outBasis[1] = 0.488603f * ny; // Y_1^-1
            outBasis[2] = 0.488603f * nz; // Y_1^0
            outBasis[3] = 0.488603f * nx; // Y_1^1

            // L2
            outBasis[4] = 1.092548f * nx * ny;              // Y_2^-2
            outBasis[5] = 1.092548f * ny * nz;              // Y_2^-1
            outBasis[6] = 0.315392f * (3.0f * nz * nz - 1); // Y_2^0
            outBasis[7] = 1.092548f * nx * nz;              // Y_2^1
            outBasis[8] = 0.546274f * (nx * nx - ny * ny);  // Y_2^2
        }

        /**
         * @brief Evaluate irradiance from SH coefficients at a given normal direction.
         *
         * Uses the Ramamoorthi & Hanrahan irradiance convolution constants
         * for L0, L1, L2 bands.
         *
         * @param sh  SH coefficients (irradiance-encoded)
         * @param nx, ny, nz  Surface normal direction
         * @param outR, outG, outB  Output irradiance per channel
         */
        static void EvaluateIrradiance(const SHCoefficients& sh, float nx, float ny, float nz, float& outR, float& outG,
                                       float& outB)
        {
            float basis[SH_COEFFICIENT_COUNT];
            EvaluateBasis(nx, ny, nz, basis);

            // Irradiance convolution weights (Ramamoorthi & Hanrahan 2001)
            static constexpr float A0 = 3.141593f; // pi
            static constexpr float A1 = 2.094395f; // 2*pi/3
            static constexpr float A2 = 0.785398f; // pi/4

            static constexpr float weights[SH_COEFFICIENT_COUNT] = {A0, A1, A1, A1, A2, A2, A2, A2, A2};

            outR = 0.0f;
            outG = 0.0f;
            outB = 0.0f;
            for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                float w = basis[i] * weights[i];
                outR += sh.r[i] * w;
                outG += sh.g[i] * w;
                outB += sh.b[i] * w;
            }

            outR = std::max(0.0f, outR);
            outG = std::max(0.0f, outG);
            outB = std::max(0.0f, outB);
        }

        /**
         * @brief Project a single directional light into SH coefficients.
         *
         * @param dirX, dirY, dirZ  Light direction (normalized, pointing toward light)
         * @param r, g, b           Light color/intensity
         * @return SH coefficients representing this directional light
         */
        static SHCoefficients ProjectDirectionalLight(float dirX, float dirY, float dirZ, float r, float g, float b)
        {
            SHCoefficients sh{};
            float basis[SH_COEFFICIENT_COUNT];
            EvaluateBasis(dirX, dirY, dirZ, basis);

            for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                sh.r[i] = basis[i] * r;
                sh.g[i] = basis[i] * g;
                sh.b[i] = basis[i] * b;
            }

            return sh;
        }

        /**
         * @brief Project ambient (constant) lighting into SH.
         *
         * Only the L0 band is used for uniform ambient.
         *
         * @param r, g, b  Ambient color/intensity
         * @return SH coefficients for constant ambient
         */
        static SHCoefficients ProjectAmbient(float r, float g, float b)
        {
            SHCoefficients sh{};
            // L0 coefficient for constant function = 1/(2*sqrt(pi))
            // Integral of constant over sphere = 4*pi, so coefficient = c * sqrt(4*pi) = c / Y_0^0
            float scale = 1.0f / 0.282095f;
            sh.r[0] = r * scale;
            sh.g[0] = g * scale;
            sh.b[0] = b * scale;
            return sh;
        }

        // =====================================================================
        // BRDF LUT Generation (Filament split-sum approximation)
        // =====================================================================

        /**
         * @brief BRDF LUT pixel (scale and bias for split-sum approximation).
         *
         * For a given (NdotV, roughness), stores:
         * - scale: multiplier for F0 (base reflectivity)
         * - bias:  additive term (Fresnel offset)
         *
         * Final specular IBL = F0 * scale + bias
         */
        struct BRDFLUTPixel
        {
            float scale = 0.0f;
            float bias = 0.0f;
        };

        /**
         * @brief Generate a BRDF integration lookup table for split-sum IBL.
         *
         * Pre-integrates the GGX BRDF against the Fresnel term over the hemisphere.
         * The result is a 2D LUT indexed by (NdotV, roughness).
         *
         * @param size  LUT dimensions (size × size)
         * @param numSamples  Importance sampling count per texel
         * @return Row-major LUT data. X = NdotV [0..1], Y = roughness [0..1].
         */
        static std::vector<BRDFLUTPixel> GenerateBRDFLUT(uint32_t size = 128, uint32_t numSamples = 256)
        {
            std::vector<BRDFLUTPixel> lut(size * size);

            for (uint32_t y = 0; y < size; ++y)
            {
                float roughness = std::max(static_cast<float>(y + 1) / static_cast<float>(size), 0.001f);
                float alpha = roughness * roughness;

                for (uint32_t x = 0; x < size; ++x)
                {
                    float NdotV = std::max(static_cast<float>(x + 1) / static_cast<float>(size), 0.001f);

                    float scaleSum = 0.0f;
                    float biasSum = 0.0f;

                    for (uint32_t i = 0; i < numSamples; ++i)
                    {
                        // Hammersley quasi-random sequence
                        float xi1 = static_cast<float>(i) / static_cast<float>(numSamples);
                        float xi2 = RadicalInverse(i);

                        // GGX importance sampling
                        float phi = 2.0f * PI * xi1;
                        float cosTheta = std::sqrt((1.0f - xi2) / (1.0f + (alpha * alpha - 1.0f) * xi2));
                        float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);

                        // Half vector in tangent space
                        float hx = sinTheta * std::cos(phi);
                        float hy = sinTheta * std::sin(phi);
                        float hz = cosTheta;

                        // View vector (tangent space, N = (0,0,1))
                        float vx = std::sqrt(1.0f - NdotV * NdotV);
                        float vy = 0.0f;
                        float vz = NdotV;

                        // Light vector = reflect(-V, H)
                        float VdotH = vx * hx + vy * hy + vz * hz;
                        float lx = 2.0f * VdotH * hx - vx;
                        float ly = 2.0f * VdotH * hy - vy;
                        float lz = 2.0f * VdotH * hz - vz;

                        float NdotL = std::max(lz, 0.0f);
                        float NdotH = std::max(hz, 0.0f);
                        VdotH = std::max(VdotH, 0.0f);

                        if (NdotL > 0.0f)
                        {
                            float G = GeometrySmith(NdotV, NdotL, roughness);
                            float Gv = G * VdotH / (NdotH * NdotV);
                            float Fc = std::pow(1.0f - VdotH, 5.0f);

                            scaleSum += Gv * (1.0f - Fc);
                            biasSum += Gv * Fc;
                        }
                    }

                    lut[y * size + x] = {scaleSum / static_cast<float>(numSamples),
                                         biasSum / static_cast<float>(numSamples)};
                }
            }

            return lut;
        }

      private:
        /// @brief Van der Corput radical inverse for Hammersley sequence
        static float RadicalInverse(uint32_t bits)
        {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return static_cast<float>(bits) * 2.3283064365386963e-10f; // / 0x100000000
        }

        /// @brief GGX geometry function (Smith's method with Schlick-GGX)
        static float GeometrySchlickGGX(float NdotV, float roughness)
        {
            float k = (roughness * roughness) / 2.0f;
            return NdotV / (NdotV * (1.0f - k) + k);
        }

        /// @brief Combined geometry term for view and light directions
        static float GeometrySmith(float NdotV, float NdotL, float roughness)
        {
            return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
        }
    };

} // namespace Spark::Graphics
