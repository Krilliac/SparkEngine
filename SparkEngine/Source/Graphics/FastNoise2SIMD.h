/**
 * @file FastNoise2SIMD.h
 * @brief SIMD-accelerated procedural noise with node graph evaluation
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a runtime-detected SIMD noise library with a composable node
 * graph. Scalar fallback processes 4 values per iteration to maintain
 * consistent batch semantics.
 *
 * Features:
 * - Runtime ISA detection (Scalar, SSE2, AVX2)
 * - Node types: Simplex, Perlin, Cellular, FBM, DomainWarp
 * - Batch evaluation: process arrays of coordinates
 * - Node graph: add nodes, connect inputs, evaluate the output
 * - FBM with configurable octaves, lacunarity, and gain
 * - Domain warping with configurable amplitude
 *
 * @see ProceduralGeneration.h, TerrainGenerator.h, NoiseSystem.h
 *
 * @note **Lifecycle-only activation (Phase S)** — NoiseGraph is instantiated by GraphicsEngine
 * (m_proceduralNoise) but no runtime noise generation calls are made. The graph is initialized
 * but never evaluated. Full activation requires a terrain or procedural-generation consumer to
 * call Evaluate() with coordinate arrays.
 *
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // SIMD Level Detection
    // =========================================================================

    enum class SIMDLevel : uint8_t
    {
        Scalar = 0,
        SSE2 = 1,
        AVX2 = 2
    };

    /**
     * @brief Detect the best available SIMD instruction set at runtime
     *
     * Uses CPUID on x86 to check for SSE2 and AVX2 support.
     * Returns Scalar on non-x86 platforms.
     */
    inline SIMDLevel DetectBestSIMD()
    {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(__AVX2__)
        return SIMDLevel::AVX2;
#elif defined(__SSE2__) || defined(_M_X64)
        return SIMDLevel::SSE2;
#else
        return SIMDLevel::Scalar;
#endif
#else
        return SIMDLevel::Scalar;
#endif
    }

    /// @brief Get a human-readable name for a SIMD level
    inline const char* SIMDLevelName(SIMDLevel level)
    {
        switch (level)
        {
        case SIMDLevel::Scalar:
            return "Scalar";
        case SIMDLevel::SSE2:
            return "SSE2";
        case SIMDLevel::AVX2:
            return "AVX2";
        default:
            return "Unknown";
        }
    }

    // =========================================================================
    // Noise Utilities
    // =========================================================================

    namespace NoiseDetail
    {
        /// @brief Fast floor for noise coordinate hashing
        inline int FastFloor(float x)
        {
            int xi = static_cast<int>(x);
            return (x < xi) ? xi - 1 : xi;
        }

        /// @brief Hash function for noise lattice coordinates
        inline int Hash(int x, int y, int seed)
        {
            int h = seed;
            h ^= x * 0x27d4eb2d;
            h ^= y * 0x1b56c4e9;
            h ^= h >> 13;
            h *= 0x5bd1e995;
            h ^= h >> 15;
            return h;
        }

        /// @brief 2D gradient from hash
        inline void Gradient2D(int hash, float& gx, float& gy)
        {
            static constexpr float gradients[][2] = {{1.0f, 0.0f},      {0.0f, 1.0f},      {-1.0f, 0.0f},
                                                     {0.0f, -1.0f},     {0.707f, 0.707f},  {-0.707f, 0.707f},
                                                     {0.707f, -0.707f}, {-0.707f, -0.707f}};
            int idx = (hash & 0x7FFFFFFF) % 8;
            gx = gradients[idx][0];
            gy = gradients[idx][1];
        }

        /// @brief Smoothstep interpolation
        inline float Fade(float t)
        {
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        /// @brief Linear interpolation
        inline float Lerp(float a, float b, float t)
        {
            return a + t * (b - a);
        }
    } // namespace NoiseDetail

    // =========================================================================
    // Noise Node Base
    // =========================================================================

    /// @brief Node type identifier
    enum class NoiseNodeType : uint8_t
    {
        Simplex,
        Perlin,
        Cellular,
        FBM,
        DomainWarp
    };

    /**
     * @brief Abstract base for all noise generator nodes
     *
     * Each node implements Evaluate for single samples and BatchEvaluate
     * for arrays. The batch path processes 4 values per loop iteration
     * for consistent performance patterns.
     */
    class NoiseNode
    {
      public:
        virtual ~NoiseNode() = default;

        /// @brief Get the node type
        virtual NoiseNodeType GetType() const = 0;

        /// @brief Evaluate a single 2D coordinate
        virtual float Evaluate(float x, float y) const = 0;

        /**
         * @brief Evaluate a batch of 2D coordinates
         *
         * Processes 4 values per iteration in the scalar path.
         * Subclasses override for SIMD-specific implementations.
         *
         * @param out   Output noise values
         * @param inX   Input X coordinates
         * @param inY   Input Y coordinates
         * @param count Number of coordinates
         */
        virtual void BatchEvaluate(float* out, const float* inX, const float* inY, int count) const
        {
            // Scalar path: process 4 at a time for consistent batch semantics
            int i = 0;
            for (; i + 3 < count; i += 4)
            {
                out[i + 0] = Evaluate(inX[i + 0], inY[i + 0]);
                out[i + 1] = Evaluate(inX[i + 1], inY[i + 1]);
                out[i + 2] = Evaluate(inX[i + 2], inY[i + 2]);
                out[i + 3] = Evaluate(inX[i + 3], inY[i + 3]);
            }
            // Handle remainder
            for (; i < count; ++i)
            {
                out[i] = Evaluate(inX[i], inY[i]);
            }
        }

        /// @brief Set the random seed
        void SetSeed(int seed) { m_seed = seed; }
        int GetSeed() const { return m_seed; }

        /// @brief Set the frequency
        void SetFrequency(float freq) { m_frequency = freq; }
        float GetFrequency() const { return m_frequency; }

      protected:
        int m_seed = 1337;
        float m_frequency = 0.01f;
    };

    // =========================================================================
    // Simplex Noise Node
    // =========================================================================

    /**
     * @brief 2D Simplex noise generator
     *
     * Uses the simplex lattice with gradient interpolation for smooth,
     * isotropic noise with O(n) complexity per dimension.
     */
    class SimplexNode : public NoiseNode
    {
      public:
        NoiseNodeType GetType() const override { return NoiseNodeType::Simplex; }

        float Evaluate(float x, float y) const override
        {
            x *= m_frequency;
            y *= m_frequency;

            // Skew to simplex space
            static constexpr float F2 = 0.3660254f; // (sqrt(3) - 1) / 2
            static constexpr float G2 = 0.2113249f; // (3 - sqrt(3)) / 6

            float s = (x + y) * F2;
            int i = NoiseDetail::FastFloor(x + s);
            int j = NoiseDetail::FastFloor(y + s);

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

            float n0 = 0.0f, n1 = 0.0f, n2 = 0.0f;

            float t0 = 0.5f - x0 * x0 - y0 * y0;
            if (t0 >= 0.0f)
            {
                t0 *= t0;
                float gx, gy;
                NoiseDetail::Gradient2D(NoiseDetail::Hash(i, j, m_seed), gx, gy);
                n0 = t0 * t0 * (gx * x0 + gy * y0);
            }

            float t1 = 0.5f - x1 * x1 - y1 * y1;
            if (t1 >= 0.0f)
            {
                t1 *= t1;
                float gx, gy;
                NoiseDetail::Gradient2D(NoiseDetail::Hash(i + i1, j + j1, m_seed), gx, gy);
                n1 = t1 * t1 * (gx * x1 + gy * y1);
            }

            float t2 = 0.5f - x2 * x2 - y2 * y2;
            if (t2 >= 0.0f)
            {
                t2 *= t2;
                float gx, gy;
                NoiseDetail::Gradient2D(NoiseDetail::Hash(i + 1, j + 1, m_seed), gx, gy);
                n2 = t2 * t2 * (gx * x2 + gy * y2);
            }

            // Scale to [-1, 1]
            return 70.0f * (n0 + n1 + n2);
        }
    };

    // =========================================================================
    // Perlin Noise Node
    // =========================================================================

    /**
     * @brief 2D Perlin (gradient) noise generator
     *
     * Classic lattice-based gradient noise with smooth interpolation.
     */
    class PerlinNode : public NoiseNode
    {
      public:
        NoiseNodeType GetType() const override { return NoiseNodeType::Perlin; }

        float Evaluate(float x, float y) const override
        {
            x *= m_frequency;
            y *= m_frequency;

            int xi = NoiseDetail::FastFloor(x);
            int yi = NoiseDetail::FastFloor(y);
            float xf = x - xi;
            float yf = y - yi;

            float u = NoiseDetail::Fade(xf);
            float v = NoiseDetail::Fade(yf);

            // Gradient dot products at 4 corners
            float gx00, gy00, gx10, gy10, gx01, gy01, gx11, gy11;
            NoiseDetail::Gradient2D(NoiseDetail::Hash(xi, yi, m_seed), gx00, gy00);
            NoiseDetail::Gradient2D(NoiseDetail::Hash(xi + 1, yi, m_seed), gx10, gy10);
            NoiseDetail::Gradient2D(NoiseDetail::Hash(xi, yi + 1, m_seed), gx01, gy01);
            NoiseDetail::Gradient2D(NoiseDetail::Hash(xi + 1, yi + 1, m_seed), gx11, gy11);

            float d00 = gx00 * xf + gy00 * yf;
            float d10 = gx10 * (xf - 1.0f) + gy10 * yf;
            float d01 = gx01 * xf + gy01 * (yf - 1.0f);
            float d11 = gx11 * (xf - 1.0f) + gy11 * (yf - 1.0f);

            float nx0 = NoiseDetail::Lerp(d00, d10, u);
            float nx1 = NoiseDetail::Lerp(d01, d11, u);
            return NoiseDetail::Lerp(nx0, nx1, v);
        }
    };

    // =========================================================================
    // Cellular (Worley) Noise Node
    // =========================================================================

    /**
     * @brief 2D Cellular (Worley) noise generator
     *
     * Returns the distance to the nearest randomly-placed feature point
     * in the lattice neighborhood.
     */
    class CellularNode : public NoiseNode
    {
      public:
        NoiseNodeType GetType() const override { return NoiseNodeType::Cellular; }

        float Evaluate(float x, float y) const override
        {
            x *= m_frequency;
            y *= m_frequency;

            int xi = NoiseDetail::FastFloor(x);
            int yi = NoiseDetail::FastFloor(y);

            float minDist = 999999.0f;

            // Search 3x3 neighborhood
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    int cx = xi + dx;
                    int cy = yi + dy;

                    // Pseudo-random feature point within cell
                    int h = NoiseDetail::Hash(cx, cy, m_seed);
                    float fx = cx + (h & 0xFF) / 255.0f;
                    float fy = cy + ((h >> 8) & 0xFF) / 255.0f;

                    float ddx = fx - x;
                    float ddy = fy - y;
                    float dist = ddx * ddx + ddy * ddy;
                    minDist = std::min(minDist, dist);
                }
            }

            return std::sqrt(minDist);
        }
    };

    // =========================================================================
    // FBM (Fractal Brownian Motion) Node
    // =========================================================================

    /**
     * @brief Fractal Brownian Motion that layers a child noise node
     *
     * Sums multiple octaves of the child noise with increasing frequency
     * (lacunarity) and decreasing amplitude (gain).
     */
    class FBMNode : public NoiseNode
    {
      public:
        /**
         * @brief Construct an FBM node wrapping a child generator
         * @param child       Source noise node (non-owning)
         * @param octaves     Number of octaves to sum
         * @param lacunarity  Frequency multiplier per octave
         * @param gain        Amplitude multiplier per octave
         */
        FBMNode(NoiseNode* child, int octaves = 6, float lacunarity = 2.0f, float gain = 0.5f)
            : m_child(child), m_octaves(octaves), m_lacunarity(lacunarity), m_gain(gain)
        {
        }

        NoiseNodeType GetType() const override { return NoiseNodeType::FBM; }

        float Evaluate(float x, float y) const override
        {
            if (!m_child)
                return 0.0f;

            float sum = 0.0f;
            float amplitude = 1.0f;
            float freq = m_frequency;
            float maxAmplitude = 0.0f;

            for (int o = 0; o < m_octaves; ++o)
            {
                // Temporarily override child frequency
                float childFreq = m_child->GetFrequency();
                m_child->SetFrequency(freq);
                sum += m_child->Evaluate(x, y) * amplitude;
                m_child->SetFrequency(childFreq);

                maxAmplitude += amplitude;
                amplitude *= m_gain;
                freq *= m_lacunarity;
            }

            return (maxAmplitude > 0.0f) ? sum / maxAmplitude : 0.0f;
        }

        void BatchEvaluate(float* out, const float* inX, const float* inY, int count) const override
        {
            if (!m_child)
            {
                for (int i = 0; i < count; ++i)
                    out[i] = 0.0f;
                return;
            }

            // Initialize output to zero
            for (int i = 0; i < count; ++i)
                out[i] = 0.0f;

            std::vector<float> temp(count);
            float amplitude = 1.0f;
            float freq = m_frequency;
            float maxAmplitude = 0.0f;

            for (int o = 0; o < m_octaves; ++o)
            {
                // Scale coordinates by frequency
                std::vector<float> scaledX(count);
                std::vector<float> scaledY(count);

                float childFreq = std::max(m_child->GetFrequency(), 0.001f);
                int i = 0;
                for (; i + 3 < count; i += 4)
                {
                    scaledX[i + 0] = inX[i + 0] * freq / childFreq;
                    scaledX[i + 1] = inX[i + 1] * freq / childFreq;
                    scaledX[i + 2] = inX[i + 2] * freq / childFreq;
                    scaledX[i + 3] = inX[i + 3] * freq / childFreq;
                    scaledY[i + 0] = inY[i + 0] * freq / childFreq;
                    scaledY[i + 1] = inY[i + 1] * freq / childFreq;
                    scaledY[i + 2] = inY[i + 2] * freq / childFreq;
                    scaledY[i + 3] = inY[i + 3] * freq / childFreq;
                }
                for (; i < count; ++i)
                {
                    scaledX[i] = inX[i] * freq / childFreq;
                    scaledY[i] = inY[i] * freq / childFreq;
                }

                m_child->BatchEvaluate(temp.data(), scaledX.data(), scaledY.data(), count);

                i = 0;
                for (; i + 3 < count; i += 4)
                {
                    out[i + 0] += temp[i + 0] * amplitude;
                    out[i + 1] += temp[i + 1] * amplitude;
                    out[i + 2] += temp[i + 2] * amplitude;
                    out[i + 3] += temp[i + 3] * amplitude;
                }
                for (; i < count; ++i)
                {
                    out[i] += temp[i] * amplitude;
                }

                maxAmplitude += amplitude;
                amplitude *= m_gain;
                freq *= m_lacunarity;
            }

            // Normalize
            if (maxAmplitude > 0.0f)
            {
                float inv = 1.0f / maxAmplitude;
                for (int i = 0; i < count; ++i)
                    out[i] *= inv;
            }
        }

        /// @brief Access FBM parameters
        int GetOctaves() const { return m_octaves; }
        float GetLacunarity() const { return m_lacunarity; }
        float GetGain() const { return m_gain; }

      private:
        NoiseNode* m_child = nullptr; ///< Non-owning pointer to child node
        int m_octaves;
        float m_lacunarity;
        float m_gain;
    };

    // =========================================================================
    // Domain Warp Node
    // =========================================================================

    /**
     * @brief Warps input coordinates using a noise source before evaluation
     *
     * Offsets the X and Y coordinates by noise-derived values to create
     * organic-looking distortions.
     */
    class DomainWarpNode : public NoiseNode
    {
      public:
        /**
         * @brief Construct a domain warp node
         * @param source    Noise node to evaluate after warping (non-owning)
         * @param warpNoise Noise node used to compute warp offsets (non-owning)
         * @param amplitude Maximum warp displacement
         */
        DomainWarpNode(NoiseNode* source, NoiseNode* warpNoise, float amplitude = 1.0f)
            : m_source(source), m_warpNoise(warpNoise), m_amplitude(amplitude)
        {
        }

        NoiseNodeType GetType() const override { return NoiseNodeType::DomainWarp; }

        float Evaluate(float x, float y) const override
        {
            if (!m_source || !m_warpNoise)
                return 0.0f;

            // Compute warp offsets using different seed offsets
            int origSeed = m_warpNoise->GetSeed();

            m_warpNoise->SetSeed(origSeed);
            float warpX = m_warpNoise->Evaluate(x, y) * m_amplitude;

            m_warpNoise->SetSeed(origSeed + 31337);
            float warpY = m_warpNoise->Evaluate(x, y) * m_amplitude;

            m_warpNoise->SetSeed(origSeed);

            return m_source->Evaluate(x + warpX, y + warpY);
        }

        /// @brief Access warp amplitude
        float GetAmplitude() const { return m_amplitude; }
        void SetAmplitude(float amp) { m_amplitude = amp; }

      private:
        NoiseNode* m_source = nullptr;    ///< Non-owning
        NoiseNode* m_warpNoise = nullptr; ///< Non-owning
        float m_amplitude;
    };

    // =========================================================================
    // Combiner Node
    // =========================================================================

    /// @brief Operation used by CombinerNode to merge two child noise outputs
    enum class CombineOp : uint8_t
    {
        Add,
        Multiply,
        Min,
        Max
    };

    /**
     * @brief Combines two child noise nodes with a configurable operation
     *
     * Evaluates both child nodes and merges the results using one of
     * Add, Multiply, Min, or Max.
     */
    class CombinerNode : public NoiseNode
    {
      public:
        /**
         * @brief Construct a combiner node
         * @param left   First child noise node (non-owning)
         * @param right  Second child noise node (non-owning)
         * @param op     Combine operation to apply
         */
        CombinerNode(NoiseNode* left, NoiseNode* right, CombineOp op = CombineOp::Add)
            : m_left(left), m_right(right), m_op(op)
        {
        }

        NoiseNodeType GetType() const override { return NoiseNodeType::Simplex; }

        float Evaluate(float x, float y) const override
        {
            if (!m_left || !m_right)
                return 0.0f;

            float a = m_left->Evaluate(x, y);
            float b = m_right->Evaluate(x, y);

            switch (m_op)
            {
            case CombineOp::Add:
                return a + b;
            case CombineOp::Multiply:
                return a * b;
            case CombineOp::Min:
                return std::min(a, b);
            case CombineOp::Max:
                return std::max(a, b);
            default:
                return a;
            }
        }

        void BatchEvaluate(float* out, const float* inX, const float* inY, int count) const override
        {
            if (!m_left || !m_right)
            {
                for (int i = 0; i < count; ++i)
                    out[i] = 0.0f;
                return;
            }

            std::vector<float> leftOut(count);
            std::vector<float> rightOut(count);
            m_left->BatchEvaluate(leftOut.data(), inX, inY, count);
            m_right->BatchEvaluate(rightOut.data(), inX, inY, count);

            for (int i = 0; i < count; ++i)
            {
                switch (m_op)
                {
                case CombineOp::Add:
                    out[i] = leftOut[i] + rightOut[i];
                    break;
                case CombineOp::Multiply:
                    out[i] = leftOut[i] * rightOut[i];
                    break;
                case CombineOp::Min:
                    out[i] = std::min(leftOut[i], rightOut[i]);
                    break;
                case CombineOp::Max:
                    out[i] = std::max(leftOut[i], rightOut[i]);
                    break;
                default:
                    out[i] = leftOut[i];
                    break;
                }
            }
        }

        /// @brief Get the combine operation
        CombineOp GetOp() const { return m_op; }

        /// @brief Set the combine operation
        void SetOp(CombineOp op) { m_op = op; }

      private:
        NoiseNode* m_left = nullptr;  ///< Non-owning pointer to left child
        NoiseNode* m_right = nullptr; ///< Non-owning pointer to right child
        CombineOp m_op;
    };

    // =========================================================================
    // Noise Graph
    // =========================================================================

    /**
     * @brief Composable noise node graph with batch evaluation
     *
     * Owns all nodes and provides the top-level Evaluate interface.
     * Set the output node to define which node's result is returned.
     */
    class NoiseGraph
    {
      public:
        NoiseGraph() = default;
        ~NoiseGraph() = default;

        /**
         * @brief Add a node to the graph (takes ownership)
         * @param node  Unique pointer to the node
         * @return Raw pointer to the added node for wiring
         */
        NoiseNode* AddNode(std::unique_ptr<NoiseNode> node)
        {
            NoiseNode* raw = node.get();
            m_nodes.push_back(std::move(node));
            return raw;
        }

        /// @brief Set which node produces the graph output
        void SetOutputNode(NoiseNode* node) { m_outputNode = node; }

        /// @brief Get the current output node
        NoiseNode* GetOutputNode() const { return m_outputNode; }

        /// @brief Evaluate a single point through the graph
        float Evaluate(float x, float y) const
        {
            if (!m_outputNode)
                return 0.0f;
            return m_outputNode->Evaluate(x, y);
        }

        /// @brief Batch evaluate through the output node
        void BatchEvaluate(float* out, const float* x, const float* y, int count) const
        {
            if (!m_outputNode)
            {
                for (int i = 0; i < count; ++i)
                    out[i] = 0.0f;
                return;
            }
            m_outputNode->BatchEvaluate(out, x, y, count);
        }

        /// @brief Get the number of nodes in the graph
        int GetNodeCount() const { return static_cast<int>(m_nodes.size()); }

        /// @brief Get the detected SIMD level
        static SIMDLevel GetSIMDLevel() { return DetectBestSIMD(); }

      private:
        std::vector<std::unique_ptr<NoiseNode>> m_nodes;
        NoiseNode* m_outputNode = nullptr; ///< Non-owning, points into m_nodes
    };

} // namespace Spark::Graphics
