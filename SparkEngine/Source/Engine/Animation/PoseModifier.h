/**
 * @file PoseModifier.h
 * @brief CryEngine-inspired post-animation pose modification pipeline
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a stack of prioritized pose modifiers that run after animation blending
 * to adjust the final skeletal pose. Common uses include look-at (head tracking),
 * aim IK (weapon aiming), and foot placement (ground alignment).
 *
 * ## Usage
 * @code
 *   PoseModifierStack stack;
 *
 *   auto lookAt = std::make_unique<LookAtModifier>();
 *   lookAt->SetTargetPosition({10.0f, 5.0f, 0.0f});
 *   lookAt->SetBoneIndex(headBoneIdx);
 *   stack.AddModifier(std::move(lookAt));
 *
 *   PoseContext pose;
 *   pose.boneTransforms = animatedPose;
 *   pose.deltaTime = dt;
 *   stack.Apply(pose);
 * @endcode
 *
 * @see AnimationSystem, IKSolver
 */

#pragma once

#include "../../Core/Platform.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Animation
{

    // =========================================================================
    // Bone Transform
    // =========================================================================

    /**
     * @brief A decomposed bone transform: position, rotation (quaternion), scale.
     */
    struct BoneTransform
    {
        XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
        XMFLOAT4 rotation = {0.0f, 0.0f, 0.0f, 1.0f}; ///< Quaternion (x, y, z, w)
        XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};
    };

    // =========================================================================
    // Pose Context
    // =========================================================================

    /**
     * @brief Mutable pose data passed through the modifier pipeline.
     *
     * Contains the current per-bone transforms that each modifier can read and
     * modify in place. Additional context (entity, delta time) is provided for
     * modifiers that need per-frame state.
     */
    struct PoseContext
    {
        std::vector<BoneTransform> boneTransforms;
        float deltaTime = 0.0f;
        uint32_t entityID = 0;
    };

    // =========================================================================
    // IPoseModifier — Base Interface
    // =========================================================================

    /**
     * @class IPoseModifier
     * @brief Abstract interface for a single pose modification operation.
     *
     * Concrete modifiers inherit from this and implement Apply() to adjust the
     * pose. Priority determines execution order (lower = earlier).
     */
    class IPoseModifier
    {
      public:
        virtual ~IPoseModifier() = default;

        /**
         * @brief Apply this modifier to the pose.
         * @param pose  The current pose context; modify boneTransforms in place.
         */
        virtual void Apply(PoseContext& pose) = 0;

        /** @brief Human-readable name for debugging. */
        virtual const char* GetName() const = 0;

        /** @brief Execution priority (lower = runs first). Default: 0. */
        virtual int GetPriority() const { return 0; }
    };

    // =========================================================================
    // LookAtModifier
    // =========================================================================

    /**
     * @class LookAtModifier
     * @brief Rotates head/spine bones to face a world-space target.
     *
     * Applies a rotation offset to the specified bone so that its local forward
     * axis points toward the target position. A weight parameter blends between
     * the original and modified rotation.
     */
    class LookAtModifier : public IPoseModifier
    {
      public:
        void Apply(PoseContext& pose) override;
        const char* GetName() const override { return "LookAt"; }
        int GetPriority() const override { return 100; }

        void SetTargetPosition(const XMFLOAT3& target) { m_target = target; }
        void SetBoneIndex(int32_t boneIndex) { m_boneIndex = boneIndex; }
        void SetWeight(float weight) { m_weight = std::clamp(weight, 0.0f, 1.0f); }
        void SetMaxAngle(float radians) { m_maxAngle = radians; }

      private:
        XMFLOAT3 m_target = {0.0f, 0.0f, 1.0f};
        int32_t m_boneIndex = -1;
        float m_weight = 1.0f;
        float m_maxAngle = 1.5708f; ///< ~90 degrees
    };

    // =========================================================================
    // AimIKModifier
    // =========================================================================

    /**
     * @class AimIKModifier
     * @brief Adjusts arm bone chain for weapon aiming toward a target.
     *
     * Operates on a two-bone chain (shoulder-elbow-hand) and rotates the bones
     * so the hand points at the aim target. Blends with the animated pose via weight.
     */
    class AimIKModifier : public IPoseModifier
    {
      public:
        void Apply(PoseContext& pose) override;
        const char* GetName() const override { return "AimIK"; }
        int GetPriority() const override { return 200; }

        void SetTargetPosition(const XMFLOAT3& target) { m_target = target; }
        void SetBoneChain(int32_t shoulder, int32_t elbow, int32_t hand);
        void SetWeight(float weight) { m_weight = std::clamp(weight, 0.0f, 1.0f); }

      private:
        XMFLOAT3 m_target = {0.0f, 0.0f, 1.0f};
        int32_t m_shoulderIndex = -1;
        int32_t m_elbowIndex = -1;
        int32_t m_handIndex = -1;
        float m_weight = 1.0f;
    };

    // =========================================================================
    // FootPlacementModifier
    // =========================================================================

    /**
     * @class FootPlacementModifier
     * @brief Adjusts foot and pelvis bones to match ground height.
     *
     * Uses provided ground heights to offset foot bones downward/upward and
     * adjusts the pelvis to compensate, preventing leg stretching on uneven terrain.
     */
    class FootPlacementModifier : public IPoseModifier
    {
      public:
        void Apply(PoseContext& pose) override;
        const char* GetName() const override { return "FootPlacement"; }
        int GetPriority() const override { return 300; }

        void SetPelvisIndex(int32_t index) { m_pelvisIndex = index; }
        void SetLeftFootIndex(int32_t index) { m_leftFootIndex = index; }
        void SetRightFootIndex(int32_t index) { m_rightFootIndex = index; }
        void SetGroundHeights(float leftGroundY, float rightGroundY);
        void SetWeight(float weight) { m_weight = std::clamp(weight, 0.0f, 1.0f); }
        void SetMaxAdjustment(float maxDelta) { m_maxAdjustment = maxDelta; }

      private:
        int32_t m_pelvisIndex = -1;
        int32_t m_leftFootIndex = -1;
        int32_t m_rightFootIndex = -1;
        float m_leftGroundY = 0.0f;
        float m_rightGroundY = 0.0f;
        float m_weight = 1.0f;
        float m_maxAdjustment = 0.5f; ///< Maximum vertical adjustment in metres
    };

    // =========================================================================
    // PoseModifierStack
    // =========================================================================

    /**
     * @class PoseModifierStack
     * @brief Ordered collection of pose modifiers applied after animation blending.
     *
     * Modifiers are sorted by priority (ascending) before application. The stack
     * owns all modifiers via unique_ptr.
     */
    class PoseModifierStack
    {
      public:
        /**
         * @brief Add a modifier to the stack. Ownership is transferred.
         * @param modifier  The modifier to add.
         */
        void AddModifier(std::unique_ptr<IPoseModifier> modifier);

        /**
         * @brief Remove a modifier by name.
         * @param name  The name returned by IPoseModifier::GetName().
         * @return True if a modifier was found and removed.
         */
        bool RemoveModifier(const char* name);

        /**
         * @brief Apply all modifiers to the pose in priority order.
         * @param pose  The pose context to modify.
         */
        void Apply(PoseContext& pose);

        /** @brief Remove all modifiers. */
        void Clear();

        /** @brief Get the number of modifiers in the stack. */
        size_t GetCount() const { return m_modifiers.size(); }

      private:
        std::vector<std::unique_ptr<IPoseModifier>> m_modifiers;
        bool m_needsSort = false;
    };

} // namespace Spark::Animation
