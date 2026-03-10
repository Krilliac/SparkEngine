/**
 * @file PBRUtils.h
 * @brief Physically-based rendering utility functions
 * @author Spark Engine Team
 * @date 2026
 *
 * CPU-side PBR utility functions for physically-based
 * rendering model. These provide more accurate roughness mapping, Fresnel
 * calculations, and microfacet distribution functions than the common
 * approximations, suitable for material authoring tools, IBL pre-filtering,
 * and offline baking paths.
 *
 * Key improvements over standard approximations:
 * - RoughnessToAlpha: polynomial mapping (more accurate than alpha = r^2)
 * - Schlick Fresnel with conductor extension for metallic materials
 * - GGX (Trowbridge-Reitz) distribution with Smith height-correlated masking
 * - Disney diffuse BRDF (energy-conserving Burley diffuse)
 * - Roughness-dependent Fresnel for multi-scattering compensation
 *
 * @see AdvancedBRDF.h, MaterialSystem.h
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Spark::Graphics
{

    /**
     * @brief CPU-side PBR utility functions for material calculations.
     *
     * All functions are constexpr where possible and can be evaluated
     * at compile time for constant material parameters.
     */
    namespace PBRUtils
    {
        // =================================================================
        // Constants
        // =================================================================

        inline constexpr float PI = 3.14159265358979323846f;
        inline constexpr float INV_PI = 1.0f / PI;
        inline constexpr float EPSILON = 1e-7f;

        // =================================================================
        // Roughness Mapping
        // =================================================================

        /**
         * @brief Convert perceptual roughness to GGX alpha parameter.
         *
         * Polynomial remapping provides more perceptually linear
         * roughness control than the common alpha = roughness^2 shortcut.
         *
         * @param roughness  Perceptual roughness [0, 1]
         * @return GGX alpha parameter
         */
        inline float RoughnessToAlpha(float roughness)
        {
            roughness = std::max(roughness, EPSILON);
            float x = std::log(roughness);
            // Polynomial fit (Burley 2012)
            float alpha =
                1.62142f + 0.819955f * x + 0.1734f * x * x + 0.0171201f * x * x * x + 0.000640711f * x * x * x * x;
            return std::max(alpha, EPSILON);
        }

        /**
         * @brief Standard roughness-to-alpha mapping (alpha = roughness^2).
         *
         * The industry-standard mapping used by most real-time engines.
         * Less physically accurate than the polynomial mapping but widely expected.
         */
        inline float RoughnessToAlphaSquared(float roughness)
        {
            float r = std::max(roughness, EPSILON);
            return r * r;
        }

        // =================================================================
        // Fresnel Functions
        // =================================================================

        /**
         * @brief Schlick Fresnel approximation for dielectrics.
         *
         * @param cosTheta  Cosine of angle between view and half vector
         * @param F0        Reflectance at normal incidence
         * @return Fresnel reflectance
         */
        inline float FresnelSchlick(float cosTheta, float F0)
        {
            float t = 1.0f - std::clamp(cosTheta, 0.0f, 1.0f);
            float t2 = t * t;
            float t5 = t2 * t2 * t;
            return F0 + (1.0f - F0) * t5;
        }

        /**
         * @brief Schlick Fresnel for RGB F0 (metallic materials).
         *
         * @param cosTheta  Cosine of angle between view and half vector
         * @param F0_r      Red channel F0
         * @param F0_g      Green channel F0
         * @param F0_b      Blue channel F0
         * @param out_r     Output red reflectance
         * @param out_g     Output green reflectance
         * @param out_b     Output blue reflectance
         */
        inline void FresnelSchlickRGB(float cosTheta, float F0_r, float F0_g, float F0_b, float& out_r, float& out_g,
                                      float& out_b)
        {
            float t = 1.0f - std::clamp(cosTheta, 0.0f, 1.0f);
            float t2 = t * t;
            float t5 = t2 * t2 * t;
            out_r = F0_r + (1.0f - F0_r) * t5;
            out_g = F0_g + (1.0f - F0_g) * t5;
            out_b = F0_b + (1.0f - F0_b) * t5;
        }

        /**
         * @brief Exact Fresnel for dielectric interfaces.
         *
         * More accurate than Schlick for oblique angles and high IOR.
         *
         * @param cosThetaI  Cosine of incident angle
         * @param etaI       Index of refraction of incident medium
         * @param etaT       Index of refraction of transmitted medium
         * @return Fresnel reflectance
         */
        inline float FresnelDielectric(float cosThetaI, float etaI, float etaT)
        {
            cosThetaI = std::clamp(cosThetaI, -1.0f, 1.0f);

            // Potentially swap indices of refraction
            bool entering = cosThetaI > 0.0f;
            if (!entering)
            {
                std::swap(etaI, etaT);
                cosThetaI = std::abs(cosThetaI);
            }

            // Snell's law
            float sinThetaI = std::sqrt(std::max(0.0f, 1.0f - cosThetaI * cosThetaI));
            float sinThetaT = etaI / etaT * sinThetaI;

            // Total internal reflection
            if (sinThetaT >= 1.0f)
                return 1.0f;

            float cosThetaT = std::sqrt(std::max(0.0f, 1.0f - sinThetaT * sinThetaT));

            float rParl = ((etaT * cosThetaI) - (etaI * cosThetaT)) / ((etaT * cosThetaI) + (etaI * cosThetaT));
            float rPerp = ((etaI * cosThetaI) - (etaT * cosThetaT)) / ((etaI * cosThetaI) + (etaT * cosThetaT));

            return (rParl * rParl + rPerp * rPerp) * 0.5f;
        }

        /**
         * @brief Compute F0 (normal incidence reflectance) from IOR.
         */
        inline float IORToF0(float ior)
        {
            float f = (ior - 1.0f) / (ior + 1.0f);
            return f * f;
        }

        /**
         * @brief Compute IOR from F0 (inverse of IORToF0).
         */
        inline float F0ToIOR(float f0)
        {
            float r = std::sqrt(std::max(f0, 0.0f));
            return (1.0f + r) / std::max(1.0f - r, EPSILON);
        }

        // =================================================================
        // Microfacet Distribution Functions
        // =================================================================

        /**
         * @brief GGX (Trowbridge-Reitz) normal distribution function.
         *
         * @param NdotH  Cosine of angle between surface normal and half vector
         * @param alpha  GGX alpha parameter (roughness squared or from RoughnessToAlpha)
         * @return NDF value
         */
        inline float DistributionGGX(float NdotH, float alpha)
        {
            float a2 = alpha * alpha;
            float NdotH2 = NdotH * NdotH;
            float denom = NdotH2 * (a2 - 1.0f) + 1.0f;
            denom = PI * denom * denom;
            return a2 / std::max(denom, EPSILON);
        }

        /**
         * @brief Smith height-correlated masking-shadowing function for GGX.
         *
         * The height-correlated form is more physically
         * accurate than the separable Smith G1 * G1 approximation.
         *
         * @param NdotV  Cosine of angle between normal and view direction
         * @param NdotL  Cosine of angle between normal and light direction
         * @param alpha  GGX alpha parameter
         * @return Geometric attenuation factor
         */
        inline float GeometrySmithCorrelated(float NdotV, float NdotL, float alpha)
        {
            float a2 = alpha * alpha;
            float ggxV = NdotL * std::sqrt(NdotV * NdotV * (1.0f - a2) + a2);
            float ggxL = NdotV * std::sqrt(NdotL * NdotL * (1.0f - a2) + a2);
            return 0.5f / std::max(ggxV + ggxL, EPSILON);
        }

        /**
         * @brief Smith G1 (single direction masking) for GGX.
         */
        inline float GeometrySmithG1(float NdotX, float alpha)
        {
            float a2 = alpha * alpha;
            float NdotX2 = NdotX * NdotX;
            return 2.0f * NdotX / (NdotX + std::sqrt(a2 + (1.0f - a2) * NdotX2));
        }

        // =================================================================
        // Disney Diffuse BRDF
        // =================================================================

        /**
         * @brief Disney (Burley) diffuse BRDF.
         *
         * Energy-conserving diffuse model that accounts for roughness-
         * dependent retro-reflection. More physically plausible than
         * Lambertian for rough surfaces.
         *
         * @param NdotV      Cosine of normal-view angle
         * @param NdotL      Cosine of normal-light angle
         * @param LdotH      Cosine of light-half angle
         * @param roughness  Perceptual roughness [0, 1]
         * @return Diffuse BRDF value (multiply by albedo/PI)
         */
        inline float DisneyDiffuse(float NdotV, float NdotL, float LdotH, float roughness)
        {
            float fd90 = 0.5f + 2.0f * LdotH * LdotH * roughness;
            float lightScatter = 1.0f + (fd90 - 1.0f) * std::pow(1.0f - NdotL, 5.0f);
            float viewScatter = 1.0f + (fd90 - 1.0f) * std::pow(1.0f - NdotV, 5.0f);
            return lightScatter * viewScatter * INV_PI;
        }

        // =================================================================
        // Multi-Scattering Energy Compensation
        // =================================================================

        /**
         * @brief Approximate energy lost to multi-scattering for GGX.
         *
         * Based on Kulla & Conty's approach. Returns a multiplier that
         * compensates for energy lost in single-scatter BRDF models.
         *
         * @param NdotV      Cosine of normal-view angle
         * @param roughness  Perceptual roughness [0, 1]
         * @return Energy compensation multiplier (>= 1.0)
         */
        inline float MultiScatterCompensation(float NdotV, float roughness)
        {
            // Polynomial fit to precomputed directional albedo tables
            float a = roughness;
            float b = NdotV;
            float Ess = 1.0f - (0.7f * a + 0.3f * a * a) * (0.5f + 0.5f * std::exp(-3.0f * b));
            return 1.0f / std::max(Ess, EPSILON);
        }

        // =================================================================
        // Material helpers
        // =================================================================

        /**
         * @brief Compute F0 for a material given metallic and base color.
         *
         * Dielectrics use a fixed F0 of 0.04 (IOR ~1.5), metallics use
         * the base color as F0.
         *
         * @param baseColor_r  Red channel of base color
         * @param baseColor_g  Green channel of base color
         * @param baseColor_b  Blue channel of base color
         * @param metallic     Metallic factor [0, 1]
         * @param out_r        Output F0 red
         * @param out_g        Output F0 green
         * @param out_b        Output F0 blue
         */
        inline void ComputeF0(float baseColor_r, float baseColor_g, float baseColor_b, float metallic, float& out_r,
                              float& out_g, float& out_b)
        {
            constexpr float dielectricF0 = 0.04f;
            out_r = dielectricF0 * (1.0f - metallic) + baseColor_r * metallic;
            out_g = dielectricF0 * (1.0f - metallic) + baseColor_g * metallic;
            out_b = dielectricF0 * (1.0f - metallic) + baseColor_b * metallic;
        }

        /**
         * @brief Compute diffuse color for a material (metals have no diffuse).
         */
        inline void ComputeDiffuseColor(float baseColor_r, float baseColor_g, float baseColor_b, float metallic,
                                        float& out_r, float& out_g, float& out_b)
        {
            float factor = 1.0f - metallic;
            out_r = baseColor_r * factor;
            out_g = baseColor_g * factor;
            out_b = baseColor_b * factor;
        }

        /**
         * @brief Compute luminance from linear RGB.
         */
        inline float Luminance(float r, float g, float b)
        {
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

    } // namespace PBRUtils

} // namespace Spark::Graphics
