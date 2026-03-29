/**
 * @file IAnimationSystem.h
 * @brief Abstract interface for the animation asset management system
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines the interface for animation clip and skeleton management. This
 * abstraction decouples engine code from the concrete AnimationManager
 * implementation, enabling access through EngineContext.
 */

#pragma once

#include "AnimationTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace Spark::Animation
{

    /**
     * @brief Abstract interface for animation asset management.
     *
     * Provides loading, caching, and lookup of animation clips and skeletons.
     * The concrete AnimationManager implements this interface.
     */
    class IAnimationSystem
    {
      public:
        virtual ~IAnimationSystem() = default;

        // ================================================================
        // Asset Loading
        // ================================================================

        /// @brief Load a skeleton from a 3D asset file (FBX, GLTF, etc.).
        /// @param filepath Path to the model file.
        /// @return Shared pointer to the loaded Skeleton.
        virtual std::shared_ptr<Skeleton> LoadSkeleton(const std::string& filepath) = 0;

        /// @brief Load all animation clips from a 3D asset file.
        /// @param filepath Path to the model/animation file.
        /// @return Vector of loaded AnimationClips.
        virtual std::vector<std::shared_ptr<AnimationClip>> LoadAnimations(const std::string& filepath) = 0;

        // ================================================================
        // Clip Registry
        // ================================================================

        /// @brief Register an animation clip by name for later lookup.
        virtual void RegisterClip(const std::string& name, std::shared_ptr<AnimationClip> clip) = 0;

        /// @brief Retrieve a cached clip by name.
        /// @return Shared pointer to the clip, or nullptr if not found.
        virtual std::shared_ptr<AnimationClip> GetClip(const std::string& name) const = 0;

        /// @brief Retrieve a cached skeleton by name.
        /// @return Shared pointer to the skeleton, or nullptr if not found.
        virtual std::shared_ptr<Skeleton> GetSkeleton(const std::string& name) const = 0;

        // ================================================================
        // Lifecycle
        // ================================================================

        /// @brief Release all cached clips and skeletons.
        virtual void Clear() = 0;
    };

} // namespace Spark::Animation
