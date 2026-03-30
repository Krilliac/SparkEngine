/**
 * @file LightProbeSystem.h
 * @brief Light probes with spherical harmonics encoding for indirect illumination
 *
 * Inspired by Cocos Engine's GI/light-probe system. Provides SH-encoded light
 * probes with auto-placement and inverse-distance weighted interpolation.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
using Float3 = DirectX::XMFLOAT3;
#else
struct Float3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
#endif

#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    /// @brief L2 spherical harmonics (9 coefficients per color channel)
    struct SphericalHarmonics
    {
        std::array<Float3, 9> coefficients{}; ///< L0(1) + L1(3) + L2(5) = 9

        /// @brief Blend two SH sets by weight
        static SphericalHarmonics Lerp(const SphericalHarmonics& a, const SphericalHarmonics& b, float t);

        /// @brief Scale all coefficients
        void Scale(float factor);

        /// @brief Add another SH set (weighted)
        void Accumulate(const SphericalHarmonics& other, float weight);
    };

    /// @brief A single light probe in the scene
    struct LightProbe
    {
        Float3 position{};
        SphericalHarmonics sh{};
        float influenceRadius = 10.0f;
        bool baked = false;
    };

    /// @brief Grid definition for auto-placement
    struct ProbeGrid
    {
        Float3 origin{};
        Float3 cellSize{1.0f, 1.0f, 1.0f};
        uint32_t countX = 4;
        uint32_t countY = 2;
        uint32_t countZ = 4;
    };

    /// @brief Manages light probes for indirect illumination
    class LightProbeSystem
    {
      public:
        static LightProbeSystem& GetInstance()
        {
            static LightProbeSystem instance;
            return instance;
        }

        /// @brief Initialize the light probe system
        bool Initialize();

        /// @brief Add a probe and return its ID
        uint32_t AddProbe(const LightProbe& probe);

        /// @brief Remove a probe by ID
        void RemoveProbe(uint32_t probeId);

        /// @brief Auto-place probes in a uniform grid
        void SetProbeGrid(const ProbeGrid& grid);

        /// @brief Auto-place probes within bounding box at given spacing
        void AutoPlaceProbes(const Float3& boundsMin, const Float3& boundsMax, float spacing);

        /// @brief Sample indirect irradiance at a world position
        SphericalHarmonics SampleIrradiance(const Float3& position) const;

        /**
         * @brief Bake a probe with sky/ground gradient SH
         *
         * Generates L2 SH coefficients representing a sky-ground gradient
         * with configurable sky and ground colors and a directional sun term.
         *
         * @param probeId  ID of the probe to bake
         */
        void BakeProbe(uint32_t probeId);

        /**
         * @brief Bake a probe with explicit light parameters
         *
         * @param probeId       ID of the probe to bake
         * @param skyColor      Sky hemisphere color (RGB)
         * @param groundColor   Ground hemisphere color (RGB)
         * @param sunDir        Normalized sun direction (pointing toward sun)
         * @param sunColor      Sun radiance (RGB)
         */
        void BakeProbe(uint32_t probeId, const Float3& skyColor, const Float3& groundColor, const Float3& sunDir,
                       const Float3& sunColor);

        /// @brief Bake all probes
        void BakeAllProbes();

        /// @brief Get total number of probes
        uint32_t GetProbeCount() const;

        /**
         * @brief GPU constant buffer layout for uploading probe SH data
         *
         * Packs 9 SH coefficients as XMFLOAT4s (RGB + padding per band).
         * Suitable for a structured buffer or constant buffer upload.
         */
        struct alignas(16) ProbeGPUData
        {
            float shR[9];      ///< SH coefficients for red channel
            float padding0[3]; ///< Pad to 48 bytes
            float shG[9];      ///< SH coefficients for green channel
            float padding1[3]; ///< Pad to 48 bytes
            float shB[9];      ///< SH coefficients for blue channel
            float padding2[3]; ///< Pad to 48 bytes
            float posX;        ///< World position X
            float posY;        ///< World position Y
            float posZ;        ///< World position Z
            float radius;      ///< Influence radius
        };

        /**
         * @brief Pack all probe data into a GPU-ready buffer
         *
         * Fills outData with ProbeGPUData structs for all probes, suitable
         * for uploading to a D3D11 structured buffer or constant buffer.
         *
         * @param outData  Output vector filled with packed probe data
         */
        void PackProbeDataForGPU(std::vector<ProbeGPUData>& outData) const;

        /// @brief Console status
        std::string Console_GetStatus() const;

        /// @brief Shutdown
        void Shutdown();

      private:
        LightProbeSystem() = default;
        ~LightProbeSystem() = default;
        LightProbeSystem(const LightProbeSystem&) = delete;
        LightProbeSystem& operator=(const LightProbeSystem&) = delete;

        std::unordered_map<uint32_t, LightProbe> m_probes;
        uint32_t m_nextProbeId = 1;
    };

} // namespace Spark::Graphics
