/**
 * @file RagdollSystem.h
 * @brief Ragdoll physics with animation blending and hit reactions
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides a dedicated ragdoll system that bridges the AnimationSystem and
 * PhysicsSystem. Supports smooth animation-to-ragdoll blending, partial
 * ragdolls (e.g. upper body reacts to hit while legs keep walking), powered
 * ragdolls (muscle simulation), and getting up from ragdoll state.
 *
 * ## Usage
 * @code
 *   RagdollSystem ragdolls;
 *   ragdolls.Initialize(physicsSystem);
 *
 *   // Create ragdoll definition for a character skeleton
 *   RagdollDefinition def;
 *   def.AddBone("Spine", RagdollBone{ShapeType::Capsule, 0.15f, 0.4f, 5.0f});
 *   def.AddBone("Head", RagdollBone{ShapeType::Sphere, 0.12f, 0.0f, 2.0f});
 *   def.AddJoint("Spine", "Head", JointLimit{-0.5f, 0.5f, 0.3f});
 *   ragdolls.RegisterDefinition("humanoid", def);
 *
 *   // On death: blend from animation to ragdoll over 0.3 seconds
 *   ragdolls.ActivateRagdoll(entity, "humanoid", 0.3f);
 *
 *   // Hit reaction: partial ragdoll on upper body
 *   ragdolls.ApplyImpulse(entity, "Spine", hitDirection * 500.0f);
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"
#include "../../Utils/OpaqueHandle.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace Spark::Animation
{

    // =============================================================================
    // RagdollBone
    // =============================================================================

    /**
 * @brief Shape type for ragdoll physics bodies.
 */
    enum class RagdollShapeType
    {
        Sphere,
        Capsule,
        Box
    };

    /**
 * @brief Physics configuration for a single ragdoll bone.
 */
    struct RagdollBone
    {
        std::string boneName; ///< Corresponding skeleton bone name
        RagdollShapeType shape = RagdollShapeType::Capsule;
        float radius = 0.1f;               ///< Collision shape radius
        float length = 0.3f;               ///< Length (capsule/box only)
        float mass = 5.0f;                 ///< Body mass in kg
        float friction = 0.5f;             ///< Surface friction
        float restitution = 0.1f;          ///< Bounciness
        float linearDamping = 0.3f;        ///< Linear velocity damping
        float angularDamping = 0.5f;       ///< Angular velocity damping
        DirectX::XMFLOAT3 offset{0, 0, 0}; ///< Local offset from bone
    };

    // =============================================================================
    // RagdollJointLimit
    // =============================================================================

    /**
 * @brief Angular limits for a ragdoll joint connecting two bones.
 */
    struct RagdollJointLimit
    {
        float twistMin = -0.5f; ///< Minimum twist angle (radians)
        float twistMax = 0.5f;  ///< Maximum twist angle (radians)
        float swingSpan = 0.3f; ///< Maximum swing cone half-angle (radians)
        float softness = 0.8f;  ///< Joint softness (0 = hard, 1 = soft)
        float damping = 0.5f;   ///< Joint damping
    };

    // =============================================================================
    // RagdollDefinition
    // =============================================================================

    /**
 * @brief Complete ragdoll setup for a character type.
 *
 * Defines which bones have physics bodies, their shapes, and the joint
 * constraints between them.
 */
    class RagdollDefinition
    {
      public:
        /**
     * @brief Add a bone to the ragdoll.
     * @param name Bone name (must match skeleton bone).
     * @param bone Physics configuration.
     */
        void AddBone(const std::string& name, const RagdollBone& bone);

        /**
     * @brief Add a joint constraint between two bones.
     * @param parentBone Parent bone name.
     * @param childBone  Child bone name.
     * @param limits     Angular limits.
     */
        void AddJoint(const std::string& parentBone, const std::string& childBone, const RagdollJointLimit& limits);

        /** @brief Get all bone definitions. */
        const std::vector<RagdollBone>& GetBones() const { return m_bones; }

        /** @brief Get all joint definitions. */
        struct JointDef
        {
            std::string parentBone;
            std::string childBone;
            RagdollJointLimit limits;
        };
        const std::vector<JointDef>& GetJoints() const { return m_joints; }

      private:
        std::vector<RagdollBone> m_bones;
        std::vector<JointDef> m_joints;
    };

    // =============================================================================
    // RagdollState
    // =============================================================================

    /**
 * @brief Current state of a ragdoll instance.
 */
    enum class RagdollState
    {
        Inactive,    ///< Not active — animation drives the skeleton
        BlendingIn,  ///< Transitioning from animation to ragdoll
        Active,      ///< Fully ragdolled — physics drives the skeleton
        BlendingOut, ///< Transitioning from ragdoll back to animation (getting up)
        Partial      ///< Only some bones are ragdolled (hit reactions)
    };

    // =============================================================================
    // RagdollInstance
    // =============================================================================

    /**
 * @brief Per-entity runtime ragdoll data.
 */
    struct RagdollInstance
    {
        uint32_t entityId = 0;      ///< Owning entity
        std::string definitionName; ///< Name of the RagdollDefinition used
        RagdollState state = RagdollState::Inactive;
        float blendTimer = 0.0f;                        ///< Current blend progress
        float blendDuration = 0.3f;                     ///< Total blend time in seconds
        float blendWeight = 0.0f;                       ///< 0 = animation, 1 = ragdoll
        std::vector<Spark::PhysicsHandle> bodyHandles;  ///< Handles to physics bodies per bone
        std::vector<Spark::PhysicsHandle> jointHandles; ///< Handles to physics joints
        std::vector<std::string> partialBones;          ///< Bones active in partial mode
    };

    // =============================================================================
    // RagdollSystem
    // =============================================================================

    /**
 * @class RagdollSystem
 * @brief Manages ragdoll instances and animation-physics blending.
 */
    class RagdollSystem
    {
      public:
        RagdollSystem();
        ~RagdollSystem() = default;

        /** @brief Initialize with reference to the physics system. */
        void Initialize();

        /**
     * @brief Update all active ragdolls.
     * @param deltaTime Frame time in seconds.
     */
        void Update(float deltaTime);

        // --- Definition management ---

        /**
     * @brief Register a ragdoll definition.
     * @param name Unique name for this ragdoll type.
     * @param def  The ragdoll definition.
     */
        void RegisterDefinition(const std::string& name, const RagdollDefinition& def);

        // --- Ragdoll activation ---

        /**
     * @brief Activate full ragdoll on an entity.
     * @param entityId     Entity to ragdoll.
     * @param defName      Name of a registered RagdollDefinition.
     * @param blendTime    Seconds to blend from animation to ragdoll.
     */
        void ActivateRagdoll(uint32_t entityId, const std::string& defName, float blendTime = 0.3f);

        /**
     * @brief Deactivate ragdoll and blend back to animation (get-up).
     * @param entityId  Entity to deactivate ragdoll on.
     * @param blendTime Seconds to blend from ragdoll to animation.
     */
        void DeactivateRagdoll(uint32_t entityId, float blendTime = 0.5f);

        /**
     * @brief Activate partial ragdoll on specific bones (hit reaction).
     * @param entityId  Entity to partially ragdoll.
     * @param defName   RagdollDefinition name.
     * @param boneNames List of bones to make ragdoll.
     * @param blendTime Blend time.
     */
        void ActivatePartialRagdoll(uint32_t entityId, const std::string& defName,
                                    const std::vector<std::string>& boneNames, float blendTime = 0.15f);

        /**
     * @brief Apply an impulse to a ragdoll bone (e.g. bullet impact).
     * @param entityId  Entity with active ragdoll.
     * @param boneName  Bone to apply impulse to.
     * @param impulse   World-space impulse vector.
     */
        void ApplyImpulse(uint32_t entityId, const std::string& boneName, const DirectX::XMFLOAT3& impulse);

        // --- Query ---

        /** @brief Check if an entity has an active ragdoll. */
        bool IsRagdollActive(uint32_t entityId) const;

        /** @brief Get the ragdoll state for an entity. */
        RagdollState GetRagdollState(uint32_t entityId) const;

        /** @brief Get the blend weight for an entity's ragdoll. */
        float GetBlendWeight(uint32_t entityId) const;

        // --- Console integration ---

        /** @brief Get ragdoll system status (console integration). */
        std::string Console_GetStatus() const;

      private:
        void UpdateBlending(RagdollInstance& instance, float deltaTime);

        std::unordered_map<std::string, RagdollDefinition> m_definitions;
        std::unordered_map<uint32_t, RagdollInstance> m_instances;
    };

} // namespace Spark::Animation
