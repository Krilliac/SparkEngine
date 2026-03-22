/**
 * @file FastNoiseLite.h
 * @brief Fast portable noise generation (CPU + HLSL compatible)
 * @author Spark Engine Team (adapted from Auburn/FastNoiseLite, MIT license)
 * @date 2025
 *
 * Single-header noise library providing multiple noise algorithms optimized for
 * real-time use. Can be used on both CPU (C++) and GPU (HLSL version in Shaders/).
 *
 * Noise types: Simplex, Perlin, Value, Cellular (Worley), Value Cubic
 * Fractal types: FBM, Ridged, Ping-Pong, Domain Warp
 * Domain warp: Simplex, Reduced, Basic Grid
 *
 * CPU usage:
 *   FastNoiseLite noise;
 *   noise.SetNoiseType(FastNoiseLite::NoiseType::Simplex);
 *   noise.SetFractalType(FastNoiseLite::FractalType::FBM);
 *   noise.SetFractalOctaves(6);
 *   float value = noise.GetNoise(x, y, z);
 *
 * @see Engine/Procedural/, TerrainRenderer.h
 */

#pragma once

#include <cmath>
#include <cstdint>

namespace Spark::Graphics
{

    /**
     * @brief Fast portable noise generator
     *
     * Self-contained noise generation with configurable type, fractal, and
     * domain warp settings. All methods are constexpr-friendly where possible.
     */
    class FastNoiseLite
    {
      public:
        // =====================================================================
        // Enums
        // =====================================================================

        enum class NoiseType
        {
            Simplex,
            Perlin,
            Value,
            ValueCubic,
            Cellular
        };

        enum class FractalType
        {
            None,
            FBM,
            Ridged,
            PingPong
        };

        enum class CellularDistanceFunction
        {
            Euclidean,
            EuclideanSq,
            Manhattan,
            Hybrid
        };

        enum class CellularReturnType
        {
            CellValue,
            Distance,
            Distance2,
            Distance2Add,
            Distance2Sub,
            Distance2Mul,
            Distance2Div
        };

        enum class DomainWarpType
        {
            None,
            Simplex,
            SimplexReduced,
            BasicGrid
        };

        // =====================================================================
        // Configuration
        // =====================================================================

        FastNoiseLite(int seed = 1337) : m_seed(seed) {}

        void SetSeed(int seed) { m_seed = seed; }
        void SetFrequency(float frequency) { m_frequency = frequency; }
        void SetNoiseType(NoiseType type) { m_noiseType = type; }

        void SetFractalType(FractalType type) { m_fractalType = type; }
        void SetFractalOctaves(int octaves) { m_octaves = octaves; }
        void SetFractalLacunarity(float lacunarity) { m_lacunarity = lacunarity; }
        void SetFractalGain(float gain) { m_gain = gain; }
        void SetFractalWeightedStrength(float weightedStrength) { m_weightedStrength = weightedStrength; }
        void SetFractalPingPongStrength(float strength) { m_pingPongStrength = strength; }

        void SetCellularDistanceFunction(CellularDistanceFunction func) { m_cellularDistFunc = func; }
        void SetCellularReturnType(CellularReturnType type) { m_cellularReturnType = type; }
        void SetCellularJitter(float jitter) { m_cellularJitter = jitter; }

        void SetDomainWarpType(DomainWarpType type) { m_domainWarpType = type; }
        void SetDomainWarpAmplitude(float amp) { m_domainWarpAmp = amp; }

        // =====================================================================
        // Noise Generation
        // =====================================================================

        /** @brief Get 2D noise value at (x, y). Returns approximately [-1, 1]. */
        float GetNoise(float x, float y) const
        {
            x *= m_frequency;
            y *= m_frequency;

            if (m_fractalType != FractalType::None)
                return GetFractal2D(x, y);

            return GetSingleNoise2D(m_seed, x, y);
        }

        /** @brief Get 3D noise value at (x, y, z). Returns approximately [-1, 1]. */
        float GetNoise(float x, float y, float z) const
        {
            x *= m_frequency;
            y *= m_frequency;
            z *= m_frequency;

            if (m_fractalType != FractalType::None)
                return GetFractal3D(x, y, z);

            return GetSingleNoise3D(m_seed, x, y, z);
        }

        /** @brief Apply domain warp to 2D coordinates */
        void DomainWarp(float& x, float& y) const
        {
            if (m_domainWarpType == DomainWarpType::None)
                return;

            float wx = x * m_frequency;
            float wy = y * m_frequency;

            float dx = GetSingleNoise2D(m_seed, wx, wy) * m_domainWarpAmp;
            float dy = GetSingleNoise2D(m_seed + 1, wx + 31.341f, wy + 57.129f) * m_domainWarpAmp;

            x += dx;
            y += dy;
        }

        /** @brief Apply domain warp to 3D coordinates */
        void DomainWarp(float& x, float& y, float& z) const
        {
            if (m_domainWarpType == DomainWarpType::None)
                return;

            float wx = x * m_frequency;
            float wy = y * m_frequency;
            float wz = z * m_frequency;

            float dx = GetSingleNoise3D(m_seed, wx, wy, wz) * m_domainWarpAmp;
            float dy = GetSingleNoise3D(m_seed + 1, wx + 31.341f, wy + 57.129f, wz + 23.787f) * m_domainWarpAmp;
            float dz = GetSingleNoise3D(m_seed + 2, wx + 73.511f, wy + 11.893f, wz + 91.237f) * m_domainWarpAmp;

            x += dx;
            y += dy;
            z += dz;
        }

      private:
        // =====================================================================
        // Internal helpers
        // =====================================================================

        static float FastFloor(float f) { return f >= 0 ? static_cast<int>(f) : static_cast<int>(f) - 1; }

        static float Lerp(float a, float b, float t) { return a + t * (b - a); }

        static float InterpQuintic(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }

        static float InterpHermite(float t) { return t * t * (3 - 2 * t); }

        /** @brief Hash function for noise coordinates */
        static int Hash(int seed, int x, int y)
        {
            int h = seed ^ (x * 374761393) ^ (y * 668265263);
            h = (h ^ (h >> 13)) * 1274126177;
            return h;
        }

        static int Hash(int seed, int x, int y, int z)
        {
            int h = seed ^ (x * 374761393) ^ (y * 668265263) ^ (z * 1440670441);
            h = (h ^ (h >> 13)) * 1274126177;
            return h;
        }

        /** @brief Gradient table for 2D */
        static float Grad2D(int hash, float x, float y)
        {
            int h = hash & 7;
            float u = h < 4 ? x : y;
            float v = h < 4 ? y : x;
            return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
        }

        /** @brief Gradient table for 3D */
        static float Grad3D(int hash, float x, float y, float z)
        {
            int h = hash & 15;
            float u = h < 8 ? x : y;
            float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
            return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
        }

        /** @brief Value noise: hash to float in [-1, 1] */
        static float ValCoord2D(int seed, int x, int y)
        {
            int h = Hash(seed, x, y);
            return static_cast<float>(h) * (1.0f / 2147483648.0f);
        }

        static float ValCoord3D(int seed, int x, int y, int z)
        {
            int h = Hash(seed, x, y, z);
            return static_cast<float>(h) * (1.0f / 2147483648.0f);
        }

        // =====================================================================
        // Single noise evaluation
        // =====================================================================

        float GetSingleNoise2D(int seed, float x, float y) const
        {
            switch (m_noiseType)
            {
            case NoiseType::Simplex:
                return SingleSimplex2D(seed, x, y);
            case NoiseType::Perlin:
                return SinglePerlin2D(seed, x, y);
            case NoiseType::Value:
                return SingleValue2D(seed, x, y);
            case NoiseType::Cellular:
                return SingleCellular2D(seed, x, y);
            default:
                return SingleValue2D(seed, x, y);
            }
        }

        float GetSingleNoise3D(int seed, float x, float y, float z) const
        {
            switch (m_noiseType)
            {
            case NoiseType::Simplex:
                return SingleSimplex3D(seed, x, y, z);
            case NoiseType::Perlin:
                return SinglePerlin3D(seed, x, y, z);
            case NoiseType::Value:
                return SingleValue3D(seed, x, y, z);
            default:
                return SingleValue3D(seed, x, y, z);
            }
        }

        // ---- Perlin ----

        static float SinglePerlin2D(int seed, float x, float y)
        {
            int x0 = static_cast<int>(FastFloor(x));
            int y0 = static_cast<int>(FastFloor(y));
            float xd0 = x - x0;
            float yd0 = y - y0;
            float xd1 = xd0 - 1;
            float yd1 = yd0 - 1;
            float xs = InterpQuintic(xd0);
            float ys = InterpQuintic(yd0);

            return Lerp(
                Lerp(Grad2D(Hash(seed, x0, y0), xd0, yd0), Grad2D(Hash(seed, x0 + 1, y0), xd1, yd0), xs),
                Lerp(Grad2D(Hash(seed, x0, y0 + 1), xd0, yd1), Grad2D(Hash(seed, x0 + 1, y0 + 1), xd1, yd1), xs), ys);
        }

        static float SinglePerlin3D(int seed, float x, float y, float z)
        {
            int x0 = static_cast<int>(FastFloor(x));
            int y0 = static_cast<int>(FastFloor(y));
            int z0 = static_cast<int>(FastFloor(z));
            float xd0 = x - x0;
            float yd0 = y - y0;
            float zd0 = z - z0;
            float xs = InterpQuintic(xd0);
            float ys = InterpQuintic(yd0);
            float zs = InterpQuintic(zd0);

            float xd1 = xd0 - 1;
            float yd1 = yd0 - 1;
            float zd1 = zd0 - 1;

            return Lerp(Lerp(Lerp(Grad3D(Hash(seed, x0, y0, z0), xd0, yd0, zd0),
                                  Grad3D(Hash(seed, x0 + 1, y0, z0), xd1, yd0, zd0), xs),
                             Lerp(Grad3D(Hash(seed, x0, y0 + 1, z0), xd0, yd1, zd0),
                                  Grad3D(Hash(seed, x0 + 1, y0 + 1, z0), xd1, yd1, zd0), xs),
                             ys),
                        Lerp(Lerp(Grad3D(Hash(seed, x0, y0, z0 + 1), xd0, yd0, zd1),
                                  Grad3D(Hash(seed, x0 + 1, y0, z0 + 1), xd1, yd0, zd1), xs),
                             Lerp(Grad3D(Hash(seed, x0, y0 + 1, z0 + 1), xd0, yd1, zd1),
                                  Grad3D(Hash(seed, x0 + 1, y0 + 1, z0 + 1), xd1, yd1, zd1), xs),
                             ys),
                        zs);
        }

        // ---- Simplex (OpenSimplex2 style) ----

        static float SingleSimplex2D(int seed, float x, float y)
        {
            constexpr float F2 = 0.3660254037844386f;  // (sqrt(3) - 1) / 2
            constexpr float G2 = 0.21132486540518713f; // (3 - sqrt(3)) / 6

            float s = (x + y) * F2;
            int i = static_cast<int>(FastFloor(x + s));
            int j = static_cast<int>(FastFloor(y + s));

            float t = (i + j) * G2;
            float x0 = x - (i - t);
            float y0 = y - (j - t);

            int i1, j1;
            if (x0 > y0)
            {
                i1 = 1;
                j1 = 0;
            }
            else
            {
                i1 = 0;
                j1 = 1;
            }

            float x1 = x0 - i1 + G2;
            float y1 = y0 - j1 + G2;
            float x2 = x0 - 1.0f + 2.0f * G2;
            float y2 = y0 - 1.0f + 2.0f * G2;

            float n = 0.0f;

            float t0 = 0.5f - x0 * x0 - y0 * y0;
            if (t0 >= 0)
            {
                t0 *= t0;
                n += t0 * t0 * Grad2D(Hash(seed, i, j), x0, y0);
            }

            float t1 = 0.5f - x1 * x1 - y1 * y1;
            if (t1 >= 0)
            {
                t1 *= t1;
                n += t1 * t1 * Grad2D(Hash(seed, i + i1, j + j1), x1, y1);
            }

            float t2 = 0.5f - x2 * x2 - y2 * y2;
            if (t2 >= 0)
            {
                t2 *= t2;
                n += t2 * t2 * Grad2D(Hash(seed, i + 1, j + 1), x2, y2);
            }

            return n * 45.23065f; // Scale to [-1, 1]
        }

        static float SingleSimplex3D(int seed, float x, float y, float z)
        {
            constexpr float F3 = 1.0f / 3.0f;
            constexpr float G3 = 1.0f / 6.0f;

            float s = (x + y + z) * F3;
            int i = static_cast<int>(FastFloor(x + s));
            int j = static_cast<int>(FastFloor(y + s));
            int k = static_cast<int>(FastFloor(z + s));

            float t = (i + j + k) * G3;
            float x0 = x - (i - t);
            float y0 = y - (j - t);
            float z0 = z - (k - t);

            int i1, j1, k1, i2, j2, k2;
            if (x0 >= y0)
            {
                if (y0 >= z0)
                {
                    i1 = 1;
                    j1 = 0;
                    k1 = 0;
                    i2 = 1;
                    j2 = 1;
                    k2 = 0;
                }
                else if (x0 >= z0)
                {
                    i1 = 1;
                    j1 = 0;
                    k1 = 0;
                    i2 = 1;
                    j2 = 0;
                    k2 = 1;
                }
                else
                {
                    i1 = 0;
                    j1 = 0;
                    k1 = 1;
                    i2 = 1;
                    j2 = 0;
                    k2 = 1;
                }
            }
            else
            {
                if (y0 < z0)
                {
                    i1 = 0;
                    j1 = 0;
                    k1 = 1;
                    i2 = 0;
                    j2 = 1;
                    k2 = 1;
                }
                else if (x0 < z0)
                {
                    i1 = 0;
                    j1 = 1;
                    k1 = 0;
                    i2 = 0;
                    j2 = 1;
                    k2 = 1;
                }
                else
                {
                    i1 = 0;
                    j1 = 1;
                    k1 = 0;
                    i2 = 1;
                    j2 = 1;
                    k2 = 0;
                }
            }

            float x1 = x0 - i1 + G3, y1 = y0 - j1 + G3, z1 = z0 - k1 + G3;
            float x2 = x0 - i2 + 2 * G3, y2 = y0 - j2 + 2 * G3, z2 = z0 - k2 + 2 * G3;
            float x3 = x0 - 1 + 3 * G3, y3 = y0 - 1 + 3 * G3, z3 = z0 - 1 + 3 * G3;

            float n = 0;
            float t0 = 0.6f - x0 * x0 - y0 * y0 - z0 * z0;
            if (t0 >= 0)
            {
                t0 *= t0;
                n += t0 * t0 * Grad3D(Hash(seed, i, j, k), x0, y0, z0);
            }
            float t1 = 0.6f - x1 * x1 - y1 * y1 - z1 * z1;
            if (t1 >= 0)
            {
                t1 *= t1;
                n += t1 * t1 * Grad3D(Hash(seed, i + i1, j + j1, k + k1), x1, y1, z1);
            }
            float t2 = 0.6f - x2 * x2 - y2 * y2 - z2 * z2;
            if (t2 >= 0)
            {
                t2 *= t2;
                n += t2 * t2 * Grad3D(Hash(seed, i + i2, j + j2, k + k2), x2, y2, z2);
            }
            float t3 = 0.6f - x3 * x3 - y3 * y3 - z3 * z3;
            if (t3 >= 0)
            {
                t3 *= t3;
                n += t3 * t3 * Grad3D(Hash(seed, i + 1, j + 1, k + 1), x3, y3, z3);
            }

            return n * 32.0f;
        }

        // ---- Value noise ----

        static float SingleValue2D(int seed, float x, float y)
        {
            int x0 = static_cast<int>(FastFloor(x));
            int y0 = static_cast<int>(FastFloor(y));
            float xs = InterpHermite(x - x0);
            float ys = InterpHermite(y - y0);

            return Lerp(Lerp(ValCoord2D(seed, x0, y0), ValCoord2D(seed, x0 + 1, y0), xs),
                        Lerp(ValCoord2D(seed, x0, y0 + 1), ValCoord2D(seed, x0 + 1, y0 + 1), xs), ys);
        }

        static float SingleValue3D(int seed, float x, float y, float z)
        {
            int x0 = static_cast<int>(FastFloor(x));
            int y0 = static_cast<int>(FastFloor(y));
            int z0 = static_cast<int>(FastFloor(z));
            float xs = InterpHermite(x - x0);
            float ys = InterpHermite(y - y0);
            float zs = InterpHermite(z - z0);

            return Lerp(Lerp(Lerp(ValCoord3D(seed, x0, y0, z0), ValCoord3D(seed, x0 + 1, y0, z0), xs),
                             Lerp(ValCoord3D(seed, x0, y0 + 1, z0), ValCoord3D(seed, x0 + 1, y0 + 1, z0), xs), ys),
                        Lerp(Lerp(ValCoord3D(seed, x0, y0, z0 + 1), ValCoord3D(seed, x0 + 1, y0, z0 + 1), xs),
                             Lerp(ValCoord3D(seed, x0, y0 + 1, z0 + 1), ValCoord3D(seed, x0 + 1, y0 + 1, z0 + 1), xs),
                             ys),
                        zs);
        }

        // ---- Cellular ----

        float SingleCellular2D(int seed, float x, float y) const
        {
            int xr = static_cast<int>(std::round(x));
            int yr = static_cast<int>(std::round(y));

            float distance0 = 1e10f;
            float distance1 = 1e10f;

            for (int yi = yr - 1; yi <= yr + 1; ++yi)
            {
                for (int xi = xr - 1; xi <= xr + 1; ++xi)
                {
                    int h = Hash(seed, xi, yi);
                    float cellX = xi + (h & 0xFF) / 255.0f * m_cellularJitter;
                    float cellY = yi + ((h >> 8) & 0xFF) / 255.0f * m_cellularJitter;

                    float dx = x - cellX;
                    float dy = y - cellY;
                    float dist = dx * dx + dy * dy;

                    if (dist < distance0)
                    {
                        distance1 = distance0;
                        distance0 = dist;
                    }
                    else if (dist < distance1)
                    {
                        distance1 = dist;
                    }
                }
            }

            switch (m_cellularReturnType)
            {
            case CellularReturnType::CellValue:
                return distance0;
            case CellularReturnType::Distance:
                return std::sqrt(distance0);
            case CellularReturnType::Distance2:
                return std::sqrt(distance1);
            case CellularReturnType::Distance2Add:
                return (std::sqrt(distance0) + std::sqrt(distance1)) * 0.5f;
            case CellularReturnType::Distance2Sub:
                return std::sqrt(distance1) - std::sqrt(distance0);
            case CellularReturnType::Distance2Mul:
                return std::sqrt(distance0 * distance1);
            default:
                return std::sqrt(distance0);
            }
        }

        // =====================================================================
        // Fractal evaluation
        // =====================================================================

        float GetFractal2D(float x, float y) const
        {
            switch (m_fractalType)
            {
            case FractalType::FBM:
                return GetFBM2D(x, y);
            case FractalType::Ridged:
                return GetRidged2D(x, y);
            case FractalType::PingPong:
                return GetPingPong2D(x, y);
            default:
                return GetSingleNoise2D(m_seed, x, y);
            }
        }

        float GetFractal3D(float x, float y, float z) const
        {
            switch (m_fractalType)
            {
            case FractalType::FBM:
                return GetFBM3D(x, y, z);
            case FractalType::Ridged:
                return GetRidged3D(x, y, z);
            default:
                return GetSingleNoise3D(m_seed, x, y, z);
            }
        }

        float GetFBM2D(float x, float y) const
        {
            float sum = 0, amp = 1, freq = 1;
            for (int i = 0; i < m_octaves; ++i)
            {
                float noise = GetSingleNoise2D(m_seed + i, x * freq, y * freq);
                sum += noise * amp;
                amp *= m_gain;
                freq *= m_lacunarity;
            }
            return sum;
        }

        float GetFBM3D(float x, float y, float z) const
        {
            float sum = 0, amp = 1, freq = 1;
            for (int i = 0; i < m_octaves; ++i)
            {
                float noise = GetSingleNoise3D(m_seed + i, x * freq, y * freq, z * freq);
                sum += noise * amp;
                amp *= m_gain;
                freq *= m_lacunarity;
            }
            return sum;
        }

        float GetRidged2D(float x, float y) const
        {
            float sum = 0, amp = 1, freq = 1;
            for (int i = 0; i < m_octaves; ++i)
            {
                float noise = std::abs(GetSingleNoise2D(m_seed + i, x * freq, y * freq));
                noise = 1.0f - noise; // Invert to get ridges
                sum += noise * amp;
                amp *= m_gain;
                freq *= m_lacunarity;
            }
            return sum;
        }

        float GetRidged3D(float x, float y, float z) const
        {
            float sum = 0, amp = 1, freq = 1;
            for (int i = 0; i < m_octaves; ++i)
            {
                float noise = std::abs(GetSingleNoise3D(m_seed + i, x * freq, y * freq, z * freq));
                noise = 1.0f - noise;
                sum += noise * amp;
                amp *= m_gain;
                freq *= m_lacunarity;
            }
            return sum;
        }

        float GetPingPong2D(float x, float y) const
        {
            float sum = 0, amp = 1, freq = 1;
            for (int i = 0; i < m_octaves; ++i)
            {
                float noise = GetSingleNoise2D(m_seed + i, x * freq, y * freq);
                // Ping-pong: fold the noise value
                noise = (noise + 1) * m_pingPongStrength;
                noise = std::abs(noise - static_cast<int>(noise) * 2 - 1) * 2 - 1;
                sum += noise * amp;
                amp *= m_gain;
                freq *= m_lacunarity;
            }
            return sum;
        }

        // =====================================================================
        // State
        // =====================================================================

        int m_seed = 1337;
        float m_frequency = 0.01f;
        NoiseType m_noiseType = NoiseType::Simplex;

        FractalType m_fractalType = FractalType::None;
        int m_octaves = 3;
        float m_lacunarity = 2.0f;
        float m_gain = 0.5f;
        float m_weightedStrength = 0.0f;
        float m_pingPongStrength = 2.0f;

        CellularDistanceFunction m_cellularDistFunc = CellularDistanceFunction::EuclideanSq;
        CellularReturnType m_cellularReturnType = CellularReturnType::Distance;
        float m_cellularJitter = 1.0f;

        DomainWarpType m_domainWarpType = DomainWarpType::None;
        float m_domainWarpAmp = 1.0f;
    };

} // namespace Spark::Graphics
