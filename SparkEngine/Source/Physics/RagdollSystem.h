/**
 * @file RagdollSystem.h
 * @brief Jolt ragdoll wrapper for physics-driven skeletal animation
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides ragdoll creation and control using Jolt's constraint system.
 * Supports hard keying (full animation control), soft keying (blended),
 * and full ragdoll (physics-driven) modes.
 *
 * @see PhysicsSystem.h, PhysicsTypes.h
 */

#pragma once

#include "../Core/Platform.h"
#include "PhysicsTypes.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class PhysicsSystem;
class PhysicsBody;
class PhysicsConstraint;

/**
 * @brief Ragdoll blend mode controlling how animation and physics interact.
 */
enum class RagdollMode
{
    Animated, ///< Full animation control (kinematic bodies follow animation)
    Blended,  ///< Blend between animation and physics (motor-driven)
    Physics   ///< Full ragdoll (dynamic bodies, physics-driven)
};

/**
 * @class Ragdoll
 * @brief Physics ragdoll composed of connected rigid bodies.
 *
 * A ragdoll is a collection of PhysicsBody instances (one per limb/bone)
 * connected by constraints (joints). It can operate in three modes:
 *
 * - **Animated**: All bodies are kinematic; game code sets transforms directly.
 * - **Blended**: Bodies are dynamic but motors drive them toward animated poses.
 * - **Physics**: Bodies are fully dynamic; gravity and collisions drive motion.
 *
 * ### Usage
 * @code
 *   RagdollDesc desc;
 *   desc.parts = { head, neck, spine, leftArm, rightArm, ... };
 *   auto ragdoll = physics.CreateRagdoll(desc);
 *
 *   // On character death:
 *   ragdoll->SetMode(RagdollMode::Physics);
 *
 *   // Each frame, read limb transforms for rendering:
 *   for (uint32_t i = 0; i < ragdoll->GetPartCount(); i++)
 *   {
 *       boneMat[i] = ragdoll->GetPartTransform(i);
 *   }
 * @endcode
 */
class Ragdoll
{
  public:
    Ragdoll(PhysicsSystem* physicsSystem, const RagdollDesc& desc);
    ~Ragdoll();

    // Non-copyable
    Ragdoll(const Ragdoll&) = delete;
    Ragdoll& operator=(const Ragdoll&) = delete;

    // =========================================================================
    // Mode control
    // =========================================================================

    /** @brief Set the ragdoll mode (Animated, Blended, Physics). */
    void SetMode(RagdollMode mode);

    /** @brief Get the current ragdoll mode. */
    RagdollMode GetMode() const { return m_mode; }

    // =========================================================================
    // Part access
    // =========================================================================

    /** @brief Get the number of ragdoll parts (bodies). */
    uint32_t GetPartCount() const;

    /** @brief Get the physics body for a specific part. */
    std::shared_ptr<PhysicsBody> GetPartBody(uint32_t partIndex) const;

    /** @brief Get the world-space transform of a part (for rendering). */
    XMMATRIX GetPartTransform(uint32_t partIndex) const;

    /** @brief Get the part name. */
    const std::string& GetPartName(uint32_t partIndex) const;

    // =========================================================================
    // Animation drive (Animated/Blended modes)
    // =========================================================================

    /**
     * @brief Set the target transform for a part (used in Animated/Blended modes).
     * @param partIndex  Index of the ragdoll part.
     * @param transform  Target world-space transform from animation.
     */
    void SetPartTargetTransform(uint32_t partIndex, const XMMATRIX& transform);

    /**
     * @brief Apply an impulse to a specific part (useful for hit reactions).
     * @param partIndex  Index of the ragdoll part hit.
     * @param impulse    Impulse vector in world space.
     * @param worldPoint Application point in world space.
     */
    void ApplyImpulse(uint32_t partIndex, const XMFLOAT3& impulse, const XMFLOAT3& worldPoint);

    /**
     * @brief Set the blend weight for Blended mode [0=physics, 1=animation].
     * @param weight  Blend weight in [0, 1].
     */
    void SetBlendWeight(float weight) { m_blendWeight = weight; }

    /** @brief Get the current blend weight. */
    float GetBlendWeight() const { return m_blendWeight; }

  private:
    PhysicsSystem* m_physicsSystem;
    RagdollMode m_mode = RagdollMode::Physics;
    float m_blendWeight = 0.5f;

    struct Part
    {
        std::string name;
        std::shared_ptr<PhysicsBody> body;
        std::shared_ptr<PhysicsConstraint> constraint; ///< Joint to parent (nullptr for root)
        int parentIndex;
    };

    std::vector<Part> m_parts;
    static const std::string s_emptyString;
};
