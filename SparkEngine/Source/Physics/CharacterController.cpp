#include "CharacterController.h"
#include "../Core/Platform.h"
/**
 * @file CharacterController.cpp
 * @brief Jolt CharacterVirtual wrapper implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "PhysicsSystem.h"
#include "../Utils/Validate.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

using namespace DirectX;

// ============================================================================
// CHARACTER CONTROLLER IMPLEMENTATION
// ============================================================================

CharacterController::CharacterController(PhysicsSystem* physicsSystem, const CharacterControllerDesc& desc)
    : m_physicsSystem(physicsSystem), m_desc(desc)
{
    if (!physicsSystem || !physicsSystem->GetJoltSystem())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Physics,
                       "CharacterController: null %s — deferring initialization to first Update()",
                       !physicsSystem ? "PhysicsSystem" : "JoltSystem");
        m_pendingInit = true;
        return;
    }

    TryInitialize();
}

bool CharacterController::TryInitialize()
{
    if (!m_physicsSystem || !m_physicsSystem->GetJoltSystem())
        return false;

    auto* joltSystem = m_physicsSystem->GetJoltSystem();

    // Create a capsule shape for the character
    // Jolt's capsule is centered at origin along Y, so we offset it upward
    float capsuleHalfHeight = (m_desc.height - 2.0f * m_desc.radius) / 2.0f;
    if (capsuleHalfHeight < 0.0f)
        capsuleHalfHeight = 0.01f;

    JPH::RefConst<JPH::Shape> capsuleShape = new JPH::CapsuleShape(capsuleHalfHeight, m_desc.radius);

    // Offset the shape so the bottom of the capsule is at the character's feet
    float shapeOffset = capsuleHalfHeight + m_desc.radius;
    JPH::RefConst<JPH::Shape> standingShape =
        new JPH::RotatedTranslatedShape(JPH::Vec3(0, shapeOffset, 0), JPH::Quat::sIdentity(), capsuleShape);

    // Configure the character
    JPH::CharacterVirtualSettings settings;
    settings.mShape = standingShape;
    settings.mMaxSlopeAngle = m_desc.maxSlopeAngle;
    settings.mMass = m_desc.mass;
    settings.mMaxStrength = m_desc.maxStrength;
    settings.mCharacterPadding = m_desc.characterPadding;
    settings.mPenetrationRecoverySpeed = m_desc.penetrationRecovery;
    settings.mPredictiveContactDistance = m_desc.predictiveContactDistance;
    settings.mUp = JPH::Vec3(m_desc.up.x, m_desc.up.y, m_desc.up.z);
    settings.mSupportingVolume =
        JPH::Plane(JPH::Vec3(m_desc.up.x, m_desc.up.y, m_desc.up.z), -capsuleHalfHeight); // Inside the character

    // Create the character (RAII-owned via unique_ptr)
    m_joltCharacter = std::make_unique<JPH::CharacterVirtual>(
        &settings, JPH::RVec3(m_desc.position.x, m_desc.position.y, m_desc.position.z),
        JPH::Quat(m_desc.rotation.x, m_desc.rotation.y, m_desc.rotation.z, m_desc.rotation.w), 0, joltSystem);

    m_pendingInit = false;
    SPARK_LOG_INFO(Spark::LogCategory::Physics, "CharacterController: deferred initialization succeeded");
    return true;
}

CharacterController::~CharacterController() = default;

void CharacterController::Update(float deltaTime, const XMFLOAT3& gravity)
{
    // Attempt deferred initialization if construction was postponed
    if (m_pendingInit)
    {
        if (!TryInitialize())
        {
            // Still not ready — give up on future retries to avoid log spam
            SPARK_LOG_ERROR(Spark::LogCategory::Physics,
                            "CharacterController: deferred init failed — disabling further retries");
            m_pendingInit = false;
            return;
        }
    }

    if (!m_joltCharacter || !m_physicsSystem || !m_physicsSystem->GetJoltSystem())
    {
        return;
    }

    auto* character = m_joltCharacter.get();
    auto* joltSystem = m_physicsSystem->GetJoltSystem();

    // Extended update handles gravity, ground detection, and sliding
    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;

    // Use the broadphase and object layer pair filters from the physics system
    JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(joltSystem->GetObjectVsBroadPhaseLayerFilter(),
                                                       1 /* MOVING layer */);
    JPH::DefaultObjectLayerFilter objectFilter(joltSystem->GetObjectLayerPairFilter(), 1 /* MOVING layer */);

    character->ExtendedUpdate(deltaTime, JPH::Vec3(gravity.x, gravity.y, gravity.z), updateSettings, broadPhaseFilter,
                              objectFilter, {}, {}, *m_physicsSystem->GetTempAllocator());
}

XMFLOAT3 CharacterController::GetPosition() const
{
    if (!m_joltCharacter)
        return m_desc.position;

    auto* character = m_joltCharacter.get();
    JPH::RVec3 pos = character->GetPosition();
    return XMFLOAT3(static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()));
}

void CharacterController::SetPosition(const XMFLOAT3& position)
{
    if (!m_joltCharacter)
        return;

    auto* character = m_joltCharacter.get();
    character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
}

XMFLOAT4 CharacterController::GetRotation() const
{
    if (!m_joltCharacter)
        return m_desc.rotation;

    auto* character = m_joltCharacter.get();
    JPH::Quat rot = character->GetRotation();
    return XMFLOAT4(rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW());
}

void CharacterController::SetRotation(const XMFLOAT4& rotation)
{
    if (!m_joltCharacter)
        return;

    auto* character = m_joltCharacter.get();
    character->SetRotation(JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w));
}

XMFLOAT3 CharacterController::GetLinearVelocity() const
{
    if (!m_joltCharacter)
        return {0, 0, 0};

    auto* character = m_joltCharacter.get();
    JPH::Vec3 vel = character->GetLinearVelocity();
    return XMFLOAT3(vel.GetX(), vel.GetY(), vel.GetZ());
}

void CharacterController::SetLinearVelocity(const XMFLOAT3& velocity)
{
    if (!m_joltCharacter)
        return;

    auto* character = m_joltCharacter.get();
    character->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

CharacterGroundState CharacterController::GetGroundState() const
{
    if (!m_joltCharacter)
        return CharacterGroundState::InAir;

    auto* character = m_joltCharacter.get();
    switch (character->GetGroundState())
    {
    case JPH::CharacterVirtual::EGroundState::OnGround:
        return CharacterGroundState::OnGround;
    case JPH::CharacterVirtual::EGroundState::OnSteepGround:
        return CharacterGroundState::OnSteepGround;
    case JPH::CharacterVirtual::EGroundState::NotSupported:
        return CharacterGroundState::NotSupported;
    case JPH::CharacterVirtual::EGroundState::InAir:
    default:
        return CharacterGroundState::InAir;
    }
}

bool CharacterController::IsOnGround() const
{
    return GetGroundState() == CharacterGroundState::OnGround;
}

XMFLOAT3 CharacterController::GetGroundNormal() const
{
    if (!m_joltCharacter)
        return {0, 1, 0};

    auto* character = m_joltCharacter.get();
    JPH::Vec3 normal = character->GetGroundNormal();
    return XMFLOAT3(normal.GetX(), normal.GetY(), normal.GetZ());
}

XMFLOAT3 CharacterController::GetGroundVelocity() const
{
    if (!m_joltCharacter)
        return {0, 0, 0};

    auto* character = m_joltCharacter.get();
    JPH::Vec3 vel = character->GetGroundVelocity();
    return XMFLOAT3(vel.GetX(), vel.GetY(), vel.GetZ());
}

XMFLOAT3 CharacterController::GetUp() const
{
    if (!m_joltCharacter)
        return m_desc.up;

    auto* character = m_joltCharacter.get();
    JPH::Vec3 up = character->GetUp();
    return XMFLOAT3(up.GetX(), up.GetY(), up.GetZ());
}

void CharacterController::SetUp(const XMFLOAT3& up)
{
    if (!m_joltCharacter)
        return;

    auto* character = m_joltCharacter.get();
    character->SetUp(JPH::Vec3(up.x, up.y, up.z));
}
