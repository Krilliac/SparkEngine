#include "RagdollSystem.h"
#include "../Core/Platform.h"
/**
 * @file RagdollSystem.cpp
 * @brief Jolt ragdoll wrapper implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "PhysicsBody.h"
#include "PhysicsSystem.h"
#include "../Utils/Validate.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

using namespace DirectX;

const std::string Ragdoll::s_emptyString;

// ============================================================================
// RAGDOLL IMPLEMENTATION
// ============================================================================

Ragdoll::Ragdoll(PhysicsSystem* physicsSystem, const RagdollDesc& desc) : m_physicsSystem(physicsSystem)
{
    if (!physicsSystem)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "Ragdoll creation failed: null PhysicsSystem");
        return;
    }

    SPARK_LOG_INFO(Spark::LogCategory::Physics, "Creating ragdoll with %zu parts", desc.parts.size());

    m_parts.reserve(desc.parts.size());

    // Create bodies for each part
    for (const auto& partDesc : desc.parts)
    {
        auto body = physicsSystem->CreateBody(partDesc.bodyDesc);
        Part part;
        part.name = partDesc.name;
        part.body = body;
        part.parentIndex = partDesc.parentIndex;
        m_parts.push_back(std::move(part));
    }

    // Create constraints between parts and their parents
    for (size_t i = 0; i < desc.parts.size(); i++)
    {
        const auto& partDesc = desc.parts[i];
        if (partDesc.parentIndex < 0 || partDesc.parentIndex >= static_cast<int>(m_parts.size()))
            continue;

        auto& child = m_parts[i];
        auto& parent = m_parts[static_cast<size_t>(partDesc.parentIndex)];

        if (!child.body || !parent.body)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Physics,
                           "Ragdoll constraint skipped: null body for part %zu (child=%p, parent=%p)", i,
                           static_cast<void*>(child.body.get()), static_cast<void*>(parent.body.get()));
            continue;
        }

        // Create constraint based on type
        switch (partDesc.constraintType)
        {
        case ConstraintType::ConeTwist:
        {
            XMMATRIX frameA =
                XMMatrixTranslation(partDesc.constraintPivot.x, partDesc.constraintPivot.y, partDesc.constraintPivot.z);
            XMMATRIX frameB = XMMatrixIdentity();
            child.constraint =
                physicsSystem->CreateConeTwistConstraint(parent.body, child.body, frameA, frameB, partDesc.swingLimit1,
                                                         partDesc.swingLimit2, partDesc.twistLimit);
            break;
        }
        case ConstraintType::Hinge:
        {
            child.constraint =
                physicsSystem->CreateHingeConstraint(parent.body, child.body, partDesc.constraintPivot, {0, 0, 0},
                                                     partDesc.constraintAxis, partDesc.constraintAxis);
            break;
        }
        case ConstraintType::Fixed:
        {
            XMMATRIX frame =
                XMMatrixTranslation(partDesc.constraintPivot.x, partDesc.constraintPivot.y, partDesc.constraintPivot.z);
            child.constraint = physicsSystem->CreateFixedConstraint(parent.body, child.body, frame, XMMatrixIdentity());
            break;
        }
        default:
        {
            // Default to point constraint
            child.constraint = physicsSystem->CreatePoint2PointConstraint(parent.body, child.body,
                                                                          partDesc.constraintPivot, {0, 0, 0});
            break;
        }
        }
    }
}

Ragdoll::~Ragdoll()
{
    if (!m_physicsSystem)
        return;

    // Remove constraints first
    for (auto& part : m_parts)
    {
        if (part.constraint)
        {
            m_physicsSystem->RemoveConstraint(part.constraint);
        }
    }

    // Remove bodies
    for (auto& part : m_parts)
    {
        if (part.body)
        {
            m_physicsSystem->RemoveBody(part.body);
        }
    }

    m_parts.clear();
}

void Ragdoll::SetMode(RagdollMode mode)
{
    SPARK_LOG_INFO(Spark::LogCategory::Physics, "Ragdoll mode transition: %d -> %d", static_cast<int>(m_mode),
                   static_cast<int>(mode));
    m_mode = mode;

    for (auto& part : m_parts)
    {
        if (!part.body)
            continue;

        switch (mode)
        {
        case RagdollMode::Animated:
            part.body->SetKinematic(true);
            break;
        case RagdollMode::Blended:
            part.body->SetKinematic(false);
            part.body->SetActive(true);
            break;
        case RagdollMode::Physics:
            part.body->SetKinematic(false);
            part.body->SetActive(true);
            break;
        }
    }
}

uint32_t Ragdoll::GetPartCount() const
{
    return static_cast<uint32_t>(m_parts.size());
}

std::shared_ptr<PhysicsBody> Ragdoll::GetPartBody(uint32_t partIndex) const
{
    if (partIndex >= m_parts.size())
        return nullptr;
    return m_parts[partIndex].body;
}

XMMATRIX Ragdoll::GetPartTransform(uint32_t partIndex) const
{
    if (partIndex >= m_parts.size() || !m_parts[partIndex].body)
        return XMMatrixIdentity();
    return m_parts[partIndex].body->GetTransform();
}

const std::string& Ragdoll::GetPartName(uint32_t partIndex) const
{
    if (partIndex >= m_parts.size())
        return s_emptyString;
    return m_parts[partIndex].name;
}

void Ragdoll::SetPartTargetTransform(uint32_t partIndex, const XMMATRIX& transform)
{
    if (partIndex >= m_parts.size() || !m_parts[partIndex].body)
        return;

    if (m_mode == RagdollMode::Animated)
    {
        m_parts[partIndex].body->SetTransform(transform);
    }
    // In Blended mode, motor-driven approach would set target velocities
    // toward the animation pose. Full motor integration requires per-constraint
    // motor setup which depends on the specific skeleton.
}

void Ragdoll::ApplyImpulse(uint32_t partIndex, const XMFLOAT3& impulse, const XMFLOAT3& worldPoint)
{
    if (partIndex >= m_parts.size() || !m_parts[partIndex].body)
        return;

    // Calculate relative position from body center
    XMFLOAT3 bodyPos = m_parts[partIndex].body->GetPosition();
    XMFLOAT3 relPos = {worldPoint.x - bodyPos.x, worldPoint.y - bodyPos.y, worldPoint.z - bodyPos.z};

    m_parts[partIndex].body->ApplyImpulse(impulse, relPos);
}
