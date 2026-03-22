#include "../Core/Platform.h"
/**
 * @file SoftBodyPhysics.cpp
 * @brief Jolt soft body wrapper implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "SoftBodyPhysics.h"
#include "PhysicsSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

JPH_SUPPRESS_WARNINGS

using namespace DirectX;

// ============================================================================
// SOFT BODY IMPLEMENTATION
// ============================================================================

SoftBody::SoftBody(PhysicsSystem* physicsSystem, const SoftBodyDesc& desc)
    : m_physicsSystem(physicsSystem), m_desc(desc)
{
    if (!physicsSystem || !physicsSystem->GetJoltSystem())
        return;

    auto* joltSystem = physicsSystem->GetJoltSystem();

    // Build shared settings (vertex positions, edges, faces)
    auto sharedSettings = new JPH::SoftBodySharedSettings;

    // Add vertices
    for (const auto& v : desc.vertices)
    {
        JPH::SoftBodySharedSettings::Vertex vert;
        vert.mPosition = JPH::Float3(v.position.x, v.position.y, v.position.z);
        vert.mInvMass = v.invMass;
        sharedSettings->mVertices.push_back(vert);
    }

    // Add edge constraints
    for (const auto& edge : desc.edges)
    {
        if (edge.vertex0 < desc.vertices.size() && edge.vertex1 < desc.vertices.size())
        {
            sharedSettings->mEdgeConstraints.push_back(
                JPH::SoftBodySharedSettings::Edge(edge.vertex0, edge.vertex1, edge.compliance));
        }
    }

    // Add faces (triangles for collision)
    for (size_t i = 0; i + 2 < desc.triangles.size(); i += 3)
    {
        sharedSettings->AddFace(
            JPH::SoftBodySharedSettings::Face(desc.triangles[i], desc.triangles[i + 1], desc.triangles[i + 2]));
    }

    // Optimize shared settings
    sharedSettings->Optimize();

    // Create soft body creation settings
    JPH::SoftBodyCreationSettings bodySettings(sharedSettings, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                               1 /* MOVING layer */);
    bodySettings.mFriction = desc.friction;
    bodySettings.mRestitution = desc.restitution;
    bodySettings.mLinearDamping = desc.linearDamping;
    bodySettings.mGravityFactor = desc.gravityFactor;
    bodySettings.mPressure = desc.pressure;
    bodySettings.mNumIterations = desc.numIterations;

    // Create the soft body
    auto& bodyInterface = joltSystem->GetBodyInterface();
    JPH::BodyID bodyID = bodyInterface.CreateAndAddSoftBody(bodySettings, JPH::EActivation::Activate);

    if (!bodyID.IsInvalid())
    {
        m_joltBodyID = bodyID.GetIndexAndSequenceNumber();
    }
}

SoftBody::~SoftBody()
{
    if (m_joltBodyID != 0 && m_physicsSystem && m_physicsSystem->GetJoltSystem())
    {
        auto& bodyInterface = m_physicsSystem->GetJoltSystem()->GetBodyInterface();
        JPH::BodyID bodyID(m_joltBodyID);
        if (bodyInterface.IsAdded(bodyID))
        {
            bodyInterface.RemoveBody(bodyID);
            bodyInterface.DestroyBody(bodyID);
        }
    }
}

uint32_t SoftBody::GetVertexCount() const
{
    return static_cast<uint32_t>(m_desc.vertices.size());
}

XMFLOAT3 SoftBody::GetVertexPosition(uint32_t index) const
{
    if (index >= GetVertexCount() || !m_physicsSystem || !m_physicsSystem->GetJoltSystem())
        return {0, 0, 0};

    auto* joltSystem = m_physicsSystem->GetJoltSystem();
    JPH::BodyID bodyID(m_joltBodyID);
    const JPH::Body* body = joltSystem->GetBodyLockInterface().TryGetBody(bodyID);
    if (!body)
        return {0, 0, 0};

    const JPH::SoftBodyMotionProperties* mp =
        static_cast<const JPH::SoftBodyMotionProperties*>(body->GetMotionProperties());
    if (!mp || index >= mp->GetVertices().size())
        return {0, 0, 0};

    JPH::Vec3 pos = mp->GetVertex(index).mPosition;
    return XMFLOAT3(pos.GetX(), pos.GetY(), pos.GetZ());
}

XMFLOAT3 SoftBody::GetVertexNormal(uint32_t index) const
{
    // Normals need to be computed from face data. Return up as default.
    (void)index;
    return {0, 1, 0};
}

void SoftBody::GetAllVertexPositions(std::vector<XMFLOAT3>& outPositions) const
{
    outPositions.resize(GetVertexCount());

    if (!m_physicsSystem || !m_physicsSystem->GetJoltSystem())
        return;

    auto* joltSystem = m_physicsSystem->GetJoltSystem();
    JPH::BodyID bodyID(m_joltBodyID);
    const JPH::Body* body = joltSystem->GetBodyLockInterface().TryGetBody(bodyID);
    if (!body)
        return;

    const JPH::SoftBodyMotionProperties* mp =
        static_cast<const JPH::SoftBodyMotionProperties*>(body->GetMotionProperties());
    if (!mp)
        return;

    const auto& verts = mp->GetVertices();
    for (size_t i = 0; i < verts.size() && i < outPositions.size(); i++)
    {
        JPH::Vec3 pos = verts[i].mPosition;
        outPositions[i] = XMFLOAT3(pos.GetX(), pos.GetY(), pos.GetZ());
    }
}

void SoftBody::PinVertex(uint32_t index)
{
    if (!m_physicsSystem || !m_physicsSystem->GetJoltSystem())
        return;

    auto* joltSystem = m_physicsSystem->GetJoltSystem();
    JPH::BodyID bodyID(m_joltBodyID);
    JPH::Body* body = const_cast<JPH::Body*>(joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!body)
        return;

    JPH::SoftBodyMotionProperties* mp = static_cast<JPH::SoftBodyMotionProperties*>(body->GetMotionProperties());
    if (mp && index < mp->GetVertices().size())
    {
        mp->GetVertex(index).mInvMass = 0.0f;
    }
}

void SoftBody::UnpinVertex(uint32_t index)
{
    if (!m_physicsSystem || !m_physicsSystem->GetJoltSystem())
        return;

    auto* joltSystem = m_physicsSystem->GetJoltSystem();
    JPH::BodyID bodyID(m_joltBodyID);
    JPH::Body* body = const_cast<JPH::Body*>(joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!body)
        return;

    JPH::SoftBodyMotionProperties* mp = static_cast<JPH::SoftBodyMotionProperties*>(body->GetMotionProperties());
    if (mp && index < mp->GetVertices().size() && index < m_desc.vertices.size())
    {
        mp->GetVertex(index).mInvMass = m_desc.vertices[index].invMass;
    }
}

void SoftBody::SetPinnedVertexPosition(uint32_t index, const XMFLOAT3& position)
{
    if (!m_physicsSystem || !m_physicsSystem->GetJoltSystem())
        return;

    auto* joltSystem = m_physicsSystem->GetJoltSystem();
    JPH::BodyID bodyID(m_joltBodyID);
    JPH::Body* body = const_cast<JPH::Body*>(joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!body)
        return;

    JPH::SoftBodyMotionProperties* mp = static_cast<JPH::SoftBodyMotionProperties*>(body->GetMotionProperties());
    if (mp && index < mp->GetVertices().size())
    {
        mp->GetVertex(index).mPosition = JPH::Vec3(position.x, position.y, position.z);
    }
}

void SoftBody::ApplyWindForce(const XMFLOAT3& windDirection, float windStrength)
{
    if (!m_physicsSystem || !m_physicsSystem->GetJoltSystem())
        return;

    auto* joltSystem = m_physicsSystem->GetJoltSystem();
    JPH::BodyID bodyID(m_joltBodyID);
    JPH::Body* body = const_cast<JPH::Body*>(joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!body)
        return;

    JPH::SoftBodyMotionProperties* mp = static_cast<JPH::SoftBodyMotionProperties*>(body->GetMotionProperties());
    if (!mp)
        return;

    JPH::Vec3 windForce(windDirection.x * windStrength, windDirection.y * windStrength, windDirection.z * windStrength);

    for (auto& vertex : mp->GetVertices())
    {
        if (vertex.mInvMass > 0.0f) // Don't apply to pinned vertices
        {
            vertex.mVelocity += windForce * vertex.mInvMass;
        }
    }
}
