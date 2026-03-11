/**
 * @file RagdollSystem.cpp
 * @brief Implementation of the ragdoll physics system
 */

#include "RagdollSystem.h"

#include <algorithm>
#include <sstream>

namespace Spark::Animation
{

    // =============================================================================
    // RagdollDefinition
    // =============================================================================

    void RagdollDefinition::AddBone(const std::string& name, const RagdollBone& bone)
    {
        RagdollBone b = bone;
        b.boneName = name;
        m_bones.push_back(std::move(b));
    }

    void RagdollDefinition::AddJoint(const std::string& parentBone, const std::string& childBone,
                                     const RagdollJointLimit& limits)
    {
        m_joints.push_back({parentBone, childBone, limits});
    }

    // =============================================================================
    // RagdollSystem
    // =============================================================================

    RagdollSystem::RagdollSystem() = default;

    void RagdollSystem::Initialize()
    {
        // Register a default humanoid ragdoll definition
        RagdollDefinition humanoid;
        humanoid.AddBone("Hips", {.shape = RagdollShapeType::Box, .radius = 0.15f, .length = 0.2f, .mass = 10.0f});
        humanoid.AddBone("Spine", {.shape = RagdollShapeType::Capsule, .radius = 0.12f, .length = 0.3f, .mass = 8.0f});
        humanoid.AddBone("Chest", {.shape = RagdollShapeType::Capsule, .radius = 0.14f, .length = 0.25f, .mass = 8.0f});
        humanoid.AddBone("Head", {.shape = RagdollShapeType::Sphere, .radius = 0.1f, .mass = 4.0f});
        humanoid.AddBone("UpperArm_L",
                         {.shape = RagdollShapeType::Capsule, .radius = 0.05f, .length = 0.25f, .mass = 3.0f});
        humanoid.AddBone("LowerArm_L",
                         {.shape = RagdollShapeType::Capsule, .radius = 0.04f, .length = 0.22f, .mass = 2.0f});
        humanoid.AddBone("UpperArm_R",
                         {.shape = RagdollShapeType::Capsule, .radius = 0.05f, .length = 0.25f, .mass = 3.0f});
        humanoid.AddBone("LowerArm_R",
                         {.shape = RagdollShapeType::Capsule, .radius = 0.04f, .length = 0.22f, .mass = 2.0f});
        humanoid.AddBone("UpperLeg_L",
                         {.shape = RagdollShapeType::Capsule, .radius = 0.07f, .length = 0.4f, .mass = 5.0f});
        humanoid.AddBone("LowerLeg_L",
                         {.shape = RagdollShapeType::Capsule, .radius = 0.05f, .length = 0.35f, .mass = 3.0f});
        humanoid.AddBone("UpperLeg_R",
                         {.shape = RagdollShapeType::Capsule, .radius = 0.07f, .length = 0.4f, .mass = 5.0f});
        humanoid.AddBone("LowerLeg_R",
                         {.shape = RagdollShapeType::Capsule, .radius = 0.05f, .length = 0.35f, .mass = 3.0f});

        humanoid.AddJoint("Hips", "Spine", {-0.3f, 0.3f, 0.2f});
        humanoid.AddJoint("Spine", "Chest", {-0.2f, 0.2f, 0.15f});
        humanoid.AddJoint("Chest", "Head", {-0.5f, 0.5f, 0.3f});
        humanoid.AddJoint("Chest", "UpperArm_L", {-1.5f, 1.5f, 1.2f});
        humanoid.AddJoint("UpperArm_L", "LowerArm_L", {0.0f, 2.5f, 0.1f});
        humanoid.AddJoint("Chest", "UpperArm_R", {-1.5f, 1.5f, 1.2f});
        humanoid.AddJoint("UpperArm_R", "LowerArm_R", {0.0f, 2.5f, 0.1f});
        humanoid.AddJoint("Hips", "UpperLeg_L", {-1.0f, 0.5f, 0.4f});
        humanoid.AddJoint("UpperLeg_L", "LowerLeg_L", {-2.2f, 0.0f, 0.1f});
        humanoid.AddJoint("Hips", "UpperLeg_R", {-1.0f, 0.5f, 0.4f});
        humanoid.AddJoint("UpperLeg_R", "LowerLeg_R", {-2.2f, 0.0f, 0.1f});

        RegisterDefinition("humanoid", humanoid);
    }

    void RagdollSystem::Update(float deltaTime)
    {
        for (auto& [entityId, instance] : m_instances)
        {
            if (instance.state == RagdollState::Inactive)
            {
                continue;
            }

            UpdateBlending(instance, deltaTime);
        }
    }

    void RagdollSystem::RegisterDefinition(const std::string& name, const RagdollDefinition& def)
    {
        m_definitions[name] = def;
    }

    void RagdollSystem::ActivateRagdoll(uint32_t entityId, const std::string& defName, float blendTime)
    {
        auto defIt = m_definitions.find(defName);
        if (defIt == m_definitions.end())
        {
            return;
        }

        RagdollInstance instance;
        instance.entityId = entityId;
        instance.definitionName = defName;
        instance.state = (blendTime > 0.0f) ? RagdollState::BlendingIn : RagdollState::Active;
        instance.blendDuration = blendTime;
        instance.blendTimer = 0.0f;
        instance.blendWeight = (blendTime > 0.0f) ? 0.0f : 1.0f;

        // Physics bodies would be created here via PhysicsSystem
        // For each bone in the definition, create a rigid body and constraint

        m_instances[entityId] = std::move(instance);
    }

    void RagdollSystem::DeactivateRagdoll(uint32_t entityId, float blendTime)
    {
        auto it = m_instances.find(entityId);
        if (it == m_instances.end())
        {
            return;
        }

        if (blendTime > 0.0f)
        {
            it->second.state = RagdollState::BlendingOut;
            it->second.blendDuration = blendTime;
            it->second.blendTimer = 0.0f;
        }
        else
        {
            it->second.state = RagdollState::Inactive;
            it->second.blendWeight = 0.0f;
        }
    }

    void RagdollSystem::ActivatePartialRagdoll(uint32_t entityId, const std::string& defName,
                                               const std::vector<std::string>& boneNames, float blendTime)
    {
        auto defIt = m_definitions.find(defName);
        if (defIt == m_definitions.end())
        {
            return;
        }

        RagdollInstance instance;
        instance.entityId = entityId;
        instance.definitionName = defName;
        instance.state = RagdollState::Partial;
        instance.blendDuration = blendTime;
        instance.blendTimer = 0.0f;
        instance.blendWeight = 0.0f;
        instance.partialBones = boneNames;

        m_instances[entityId] = std::move(instance);
    }

    void RagdollSystem::ApplyImpulse(uint32_t entityId, const std::string& boneName, const DirectX::XMFLOAT3& impulse)
    {
        auto it = m_instances.find(entityId);
        if (it == m_instances.end() || it->second.state == RagdollState::Inactive)
        {
            return;
        }

        // Find the physics body for the named bone and apply impulse
        // This would call PhysicsSystem::ApplyImpulse with the body handle
        (void)boneName;
        (void)impulse;
    }

    bool RagdollSystem::IsRagdollActive(uint32_t entityId) const
    {
        auto it = m_instances.find(entityId);
        return it != m_instances.end() && it->second.state != RagdollState::Inactive;
    }

    RagdollState RagdollSystem::GetRagdollState(uint32_t entityId) const
    {
        auto it = m_instances.find(entityId);
        return it != m_instances.end() ? it->second.state : RagdollState::Inactive;
    }

    float RagdollSystem::GetBlendWeight(uint32_t entityId) const
    {
        auto it = m_instances.find(entityId);
        return it != m_instances.end() ? it->second.blendWeight : 0.0f;
    }

    void RagdollSystem::UpdateBlending(RagdollInstance& instance, float deltaTime)
    {
        if (instance.state == RagdollState::BlendingIn)
        {
            instance.blendTimer += deltaTime;
            float t = instance.blendTimer / instance.blendDuration;
            instance.blendWeight = std::clamp(t, 0.0f, 1.0f);

            if (instance.blendTimer >= instance.blendDuration)
            {
                instance.state = RagdollState::Active;
                instance.blendWeight = 1.0f;
            }
        }
        else if (instance.state == RagdollState::BlendingOut)
        {
            instance.blendTimer += deltaTime;
            float t = instance.blendTimer / instance.blendDuration;
            instance.blendWeight = std::clamp(1.0f - t, 0.0f, 1.0f);

            if (instance.blendTimer >= instance.blendDuration)
            {
                instance.state = RagdollState::Inactive;
                instance.blendWeight = 0.0f;
            }
        }
        else if (instance.state == RagdollState::Partial)
        {
            instance.blendTimer += deltaTime;
            float t = instance.blendTimer / instance.blendDuration;
            instance.blendWeight = std::clamp(t, 0.0f, 1.0f);

            // Auto-deactivate partial ragdoll after a timeout
            if (instance.blendTimer >= instance.blendDuration + 1.0f)
            {
                instance.state = RagdollState::BlendingOut;
                instance.blendDuration = 0.3f;
                instance.blendTimer = 0.0f;
            }
        }
    }

    std::string RagdollSystem::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Ragdoll System ===\n";
        oss << "Definitions: " << m_definitions.size() << "\n";
        for (const auto& [name, def] : m_definitions)
        {
            oss << "  " << name << ": " << def.GetBones().size() << " bones, " << def.GetJoints().size() << " joints\n";
        }
        oss << "Active instances: " << m_instances.size() << "\n";
        for (const auto& [entityId, instance] : m_instances)
        {
            const char* stateNames[] = {"Inactive", "BlendingIn", "Active", "BlendingOut", "Partial"};
            oss << "  Entity " << entityId << ": " << stateNames[static_cast<int>(instance.state)]
                << " (blend=" << instance.blendWeight << ")\n";
        }
        return oss.str();
    }

} // namespace Spark::Animation
