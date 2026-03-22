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

    // Add skinned constraints (attach cloth vertices to skeleton bones)
    if (!desc.skinnedConstraints.empty() && !desc.invBindMatrices.empty())
    {
        // Add inverse bind matrices
        for (const auto& invBind : desc.invBindMatrices)
        {
            JPH::SoftBodySharedSettings::InvBind ib;
            // Convert from our column-major float[16] to Jolt's Mat44
            ib.mJointIndex = static_cast<uint32_t>(&invBind - desc.invBindMatrices.data());
            ib.mInvBind =
                JPH::Mat44(JPH::Vec4(invBind.matrix[0], invBind.matrix[1], invBind.matrix[2], invBind.matrix[3]),
                           JPH::Vec4(invBind.matrix[4], invBind.matrix[5], invBind.matrix[6], invBind.matrix[7]),
                           JPH::Vec4(invBind.matrix[8], invBind.matrix[9], invBind.matrix[10], invBind.matrix[11]),
                           JPH::Vec4(invBind.matrix[12], invBind.matrix[13], invBind.matrix[14], invBind.matrix[15]));
            sharedSettings->mInvBindMatrices.push_back(ib);
        }

        // Add skinned vertex constraints
        for (const auto& sc : desc.skinnedConstraints)
        {
            JPH::SoftBodySharedSettings::Skinned skinned(sc.vertexIndex, sc.maxDistance, sc.backStopDistance,
                                                         sc.backStopRadius);
            for (int w = 0; w < 4; w++)
            {
                if (sc.weights[w].weight <= 0.0f)
                    break;
                skinned.mWeights[w] =
                    JPH::SoftBodySharedSettings::SkinWeight(sc.weights[w].boneIndex, sc.weights[w].weight);
            }
            skinned.NormalizeWeights();
            sharedSettings->mSkinnedConstraints.push_back(skinned);
        }

        // Calculate normals needed for backstop constraints
        sharedSettings->CalculateSkinnedConstraintNormals();
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
    if (index >= GetVertexCount() || m_desc.triangles.size() < 3 || !m_physicsSystem ||
        !m_physicsSystem->GetJoltSystem())
        return {0, 1, 0};

    // Compute smooth vertex normal by averaging face normals of all triangles containing this vertex
    XMFLOAT3 normal = {0, 0, 0};
    int faceCount = 0;

    for (size_t i = 0; i + 2 < m_desc.triangles.size(); i += 3)
    {
        if (m_desc.triangles[i] == index || m_desc.triangles[i + 1] == index || m_desc.triangles[i + 2] == index)
        {
            XMFLOAT3 v0 = GetVertexPosition(m_desc.triangles[i]);
            XMFLOAT3 v1 = GetVertexPosition(m_desc.triangles[i + 1]);
            XMFLOAT3 v2 = GetVertexPosition(m_desc.triangles[i + 2]);

            // Cross product of edges to get face normal
            float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
            float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
            normal.x += e1y * e2z - e1z * e2y;
            normal.y += e1z * e2x - e1x * e2z;
            normal.z += e1x * e2y - e1y * e2x;
            faceCount++;
        }
    }

    if (faceCount == 0)
        return {0, 1, 0};

    // Normalize
    float len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len > 1e-6f)
    {
        normal.x /= len;
        normal.y /= len;
        normal.z /= len;
    }
    else
    {
        return {0, 1, 0};
    }

    return normal;
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

void SoftBody::UpdateSkeletonMatrices(const float* matrices, uint32_t count)
{
    if (!m_physicsSystem || !m_physicsSystem->GetJoltSystem() || !matrices || count == 0)
        return;

    auto* joltSystem = m_physicsSystem->GetJoltSystem();
    JPH::BodyID bodyID(m_joltBodyID);
    JPH::Body* body = const_cast<JPH::Body*>(joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!body)
        return;

    JPH::SoftBodyMotionProperties* mp = static_cast<JPH::SoftBodyMotionProperties*>(body->GetMotionProperties());
    if (!mp)
        return;

    // Build array of Jolt RMat44 from flat float arrays (each 16 floats, column-major)
    JPH::Array<JPH::RMat44> joltMatrices;
    joltMatrices.reserve(count);
    for (uint32_t i = 0; i < count; i++)
    {
        const float* m = matrices + i * 16;
        joltMatrices.push_back(JPH::RMat44(JPH::Vec4(m[0], m[1], m[2], m[3]), JPH::Vec4(m[4], m[5], m[6], m[7]),
                                           JPH::Vec4(m[8], m[9], m[10], m[11]), JPH::Vec3(m[12], m[13], m[14])));
    }

    JPH::RMat44 comTransform = body->GetCenterOfMassTransform();
    mp->SkinVertices(comTransform, joltMatrices.data(), static_cast<uint32_t>(joltMatrices.size()), true,
                     *m_physicsSystem->GetTempAllocator());
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
