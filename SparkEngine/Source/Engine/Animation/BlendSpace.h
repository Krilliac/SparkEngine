/**
 * @file BlendSpace.h
 * @brief Parametric 2D blend space for animation blending
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a 2D blend space that maps a two-parameter input (e.g. speed and
 * direction) to a weighted combination of animation clips. Samples are placed
 * in a parameter-space grid, triangulated via Delaunay, and evaluated using
 * barycentric interpolation to produce smooth blending weights.
 *
 * ## Usage
 * @code
 *   auto& mgr = BlendSpaceManager::GetInstance();
 *   auto& locomotion = mgr.CreateBlendSpace("Locomotion");
 *
 *   locomotion.AddSample({0.0f, 0.0f}, "Idle", 1.0f);
 *   locomotion.AddSample({1.0f, 0.0f}, "WalkForward", 1.0f);
 *   locomotion.AddSample({2.0f, 0.0f}, "RunForward", 1.2f);
 *   locomotion.AddSample({1.0f, -1.0f}, "WalkLeft", 1.0f);
 *   locomotion.AddSample({1.0f, 1.0f},  "WalkRight", 1.0f);
 *
 *   BlendSpaceResult result = locomotion.Evaluate(currentSpeed, currentDirection);
 *   // result.animations contains weighted clip references summing to 1.0
 * @endcode
 *
 * @see AnimationSystem, AnimationBlender
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Animation
{

    // =========================================================================
    // Blend Space Sample
    // =========================================================================

    /**
     * @brief A single sample point in the 2D blend space.
     *
     * Each sample associates a position in parameter space with a named
     * animation clip and an optional playback speed override.
     */
    struct BlendSpaceSample
    {
        XMFLOAT2 position = {0.0f, 0.0f}; ///< Position in parameter space
        std::string animationName;        ///< Name of the animation clip
        float playbackSpeed = 1.0f;       ///< Playback speed multiplier
    };

    // =========================================================================
    // Blend Space Result
    // =========================================================================

    /**
     * @brief A single weighted animation contribution in a blend result.
     */
    struct WeightedAnimation
    {
        std::string animationName;
        float weight = 0.0f;
        float playbackSpeed = 1.0f;
    };

    /**
     * @brief The result of evaluating a blend space at a parameter position.
     *
     * Contains 1-3 weighted animations whose weights sum to approximately 1.0.
     * The consumer blends these clips together using the provided weights.
     */
    struct BlendSpaceResult
    {
        std::vector<WeightedAnimation> animations;
    };

    // =========================================================================
    // Triangle (internal Delaunay structure)
    // =========================================================================

    /**
     * @brief A triangle in the Delaunay triangulation, referencing sample indices.
     */
    struct BlendTriangle
    {
        uint32_t indices[3] = {0, 0, 0};
    };

    // =========================================================================
    // BlendSpace2D
    // =========================================================================

    /**
     * @class BlendSpace2D
     * @brief A 2D parametric blend space for animation interpolation.
     *
     * Samples are placed in a 2D parameter space (e.g. X = speed, Y = turn angle).
     * When evaluated at a point, the containing Delaunay triangle is found and
     * barycentric coordinates are used to compute weights for the three vertices.
     */
    class BlendSpace2D
    {
      public:
        /**
         * @brief Construct a named blend space.
         * @param name  Human-readable name for identification.
         */
        explicit BlendSpace2D(const std::string& name);

        /**
         * @brief Add a sample to the blend space.
         * @param position       Parameter-space position (X, Y).
         * @param animName       Name of the animation clip.
         * @param playbackSpeed  Playback speed multiplier (default: 1.0).
         */
        void AddSample(const XMFLOAT2& position, const std::string& animName, float playbackSpeed = 1.0f);

        /**
         * @brief Remove a sample by animation name.
         * @param animName  Name of the clip to remove.
         * @return True if a sample was found and removed.
         */
        bool RemoveSample(const std::string& animName);

        /**
         * @brief Evaluate the blend space at a parameter position.
         *
         * Finds the Delaunay triangle containing (paramX, paramY) and computes
         * barycentric weights for the three corner samples. If only 1-2 samples
         * exist, falls back to nearest-sample or edge interpolation.
         *
         * @param paramX  X parameter value.
         * @param paramY  Y parameter value.
         * @return        Weighted animation list (typically 1-3 entries summing to 1.0).
         */
        BlendSpaceResult Evaluate(float paramX, float paramY) const;

        /** @brief Get the blend space name. */
        const std::string& GetName() const { return m_name; }

        /** @brief Get the number of samples. */
        size_t GetSampleCount() const { return m_samples.size(); }

        /** @brief Get all samples (read-only). */
        const std::vector<BlendSpaceSample>& GetSamples() const { return m_samples; }

      private:
        /** @brief Rebuild the Delaunay triangulation from current samples. */
        void Retriangulate();

        /**
         * @brief Find the triangle containing a point and compute barycentric coords.
         * @param px, py      Query point in parameter space.
         * @param outWeights   Output barycentric weights (3 values).
         * @return             Index of the containing triangle, or -1 if none found.
         */
        int FindContainingTriangle(float px, float py, float outWeights[3]) const;

        /**
         * @brief Find the nearest sample to a point (fallback for degenerate cases).
         * @param px, py  Query point.
         * @return        Index of the nearest sample.
         */
        size_t FindNearestSample(float px, float py) const;

        /**
         * @brief Compute barycentric coordinates of point P in triangle (A, B, C).
         * @return True if point is inside the triangle.
         */
        static bool ComputeBarycentric(float px, float py, float ax, float ay, float bx, float by, float cx, float cy,
                                       float& u, float& v, float& w);

        std::string m_name;
        std::vector<BlendSpaceSample> m_samples;
        std::vector<BlendTriangle> m_triangles;
        bool m_dirty = true;
    };

    // =========================================================================
    // BlendSpaceManager
    // =========================================================================

    /**
     * @class BlendSpaceManager
     * @brief Singleton registry of named 2D blend spaces.
     */
    class BlendSpaceManager
    {
      public:
        /** @brief Singleton access. */
        static BlendSpaceManager& GetInstance()
        {
            static BlendSpaceManager s;
            return s;
        }

        /**
         * @brief Create a new blend space with the given name.
         * @param name  Unique name for the blend space.
         * @return      Reference to the newly created blend space.
         */
        BlendSpace2D& CreateBlendSpace(const std::string& name);

        /**
         * @brief Look up a blend space by name.
         * @param name  Name of the blend space.
         * @return      Pointer to the blend space, or nullptr if not found.
         */
        BlendSpace2D* GetBlendSpace(const std::string& name);

        /**
         * @brief Remove a blend space by name.
         * @param name  Name of the blend space to remove.
         * @return      True if found and removed.
         */
        bool RemoveBlendSpace(const std::string& name);

        /** @brief Get the number of registered blend spaces. */
        size_t GetCount() const { return m_blendSpaces.size(); }

      private:
        BlendSpaceManager() = default;
        ~BlendSpaceManager() = default;
        BlendSpaceManager(const BlendSpaceManager&) = delete;
        BlendSpaceManager& operator=(const BlendSpaceManager&) = delete;

        std::unordered_map<std::string, BlendSpace2D> m_blendSpaces;
    };

} // namespace Spark::Animation
