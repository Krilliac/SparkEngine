#include "../Core/Platform.h"
/**
 * @file CharacterController.cpp
 * @brief Jolt CharacterVirtual wrapper implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "CharacterController.h"
#include "PhysicsSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

JPH_SUPPRESS_WARNINGS

using namespace DirectX;

// ============================================================================
// CHARACTER CONTROLLER IMPLEMENTATION
// ============================================================================

CharacterController::CharacterController(PhysicsSystem* physicsSystem, const CharacterControllerDesc& desc)
    : m_physicsSystem(physicsSystem), m_desc(desc)
{
    if (!physicsSystem || !physicsSystem->GetJoltSystem())
        return;

    auto* joltSystem = physicsSystem->GetJoltSystem();

    // Create a capsule shape for the character
    // Jolt's capsule is centered at origin along Y, so we offset it upward
    float capsuleHalfHeight = (desc.height - 2.0f * desc.radius) / 2.0f;
    if (capsuleHalfHeight < 0.0f)
        capsuleHalfHeight = 0.01f;

    JPH::RefConst<JPH::Shape> capsuleShape = new JPH::CapsuleShape(capsuleHalfHeight, desc.radius);

    // Offset the shape so the bottom of the capsule is at the character's feet
    float shapeOffset = capsuleHalfHeight + desc.radius;
    JPH::RefConst<JPH::Shape> standingShape =
        new JPH::RotatedTranslatedShape(JPH::Vec3(0, shapeOffset, 0), JPH::Quat::sIdentity(), capsuleShape);

    // Configure the character
    JPH::CharacterVirtualSettings settings;
    settings.mShape = standingShape;
    settings.mMaxSlopeAngle = desc.maxSlopeAngle;
    settings.mMass = desc.mass;
    settings.mMaxStrength = desc.maxStrength;
    settings.mCharacterPadding = desc.characterPadding;
    settings.mPenetrationRecoverySpeed = desc.penetrationRecovery;
    settings.mPredictiveContactDistance = desc.predictiveContactDistance;
    settings.mUp = JPH::Vec3(desc.up.x, desc.up.y, desc.up.z);
    settings.mSupportingVolume =
        JPH::Plane(JPH::Vec3(desc.up.x, desc.up.y, desc.up.z), -capsuleHalfHeight); // Inside the character

    // Create the character
    m_joltCharacter = new JPH::CharacterVirtual(
        &settings, JPH::RVec3(desc.position.x, desc.position.y, desc.position.z),
        JPH::Quat(desc.rotation.x, desc.rotation.y, desc.rotation.z, desc.rotation.w), 0, joltSystem);
}

CharacterController::~CharacterController()
{
    delete m_joltCharacter;
    m_joltCharacter = nullptr;
}

void CharacterController::Update(float deltaTime, const XMFLOAT3& gravity)
{
    if (!m_joltCharacter || !m_physicsSystem || !m_physicsSystem->GetJoltSystem())
        return;

    auto* character = m_joltCharacter;
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

    auto* character = m_joltCharacter;
    JPH::RVec3 pos = character->GetPosition();
    return XMFLOAT3(static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()));
}

void CharacterController::SetPosition(const XMFLOAT3& position)
{
    if (!m_joltCharacter)
        return;

    auto* character = m_joltCharacter;
    character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
}

XMFLOAT4 CharacterController::GetRotation() const
{
    if (!m_joltCharacter)
        return m_desc.rotation;

    auto* character = m_joltCharacter;
    JPH::Quat rot = character->GetRotation();
    return XMFLOAT4(rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW());
}

void CharacterController::SetRotation(const XMFLOAT4& rotation)
{
    if (!m_joltCharacter)
        return;

    auto* character = m_joltCharacter;
    character->SetRotation(JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w));
}

XMFLOAT3 CharacterController::GetLinearVelocity() const
{
    if (!m_joltCharacter)
        return {0, 0, 0};

    auto* character = m_joltCharacter;
    JPH::Vec3 vel = character->GetLinearVelocity();
    return XMFLOAT3(vel.GetX(), vel.GetY(), vel.GetZ());
}

void CharacterController::SetLinearVelocity(const XMFLOAT3& velocity)
{
    if (!m_joltCharacter)
        return;

    auto* character = m_joltCharacter;
    character->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

CharacterGroundState CharacterController::GetGroundState() const
{
    if (!m_joltCharacter)
        return CharacterGroundState::InAir;

    auto* character = m_joltCharacter;
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

    auto* character = m_joltCharacter;
    JPH::Vec3 normal = character->GetGroundNormal();
    return XMFLOAT3(normal.GetX(), normal.GetY(), normal.GetZ());
}

XMFLOAT3 CharacterController::GetGroundVelocity() const
{
    if (!m_joltCharacter)
        return {0, 0, 0};

    auto* character = m_joltCharacter;
    JPH::Vec3 vel = character->GetGroundVelocity();
    return XMFLOAT3(vel.GetX(), vel.GetY(), vel.GetZ());
}

XMFLOAT3 CharacterController::GetUp() const
{
    if (!m_joltCharacter)
        return m_desc.up;

    auto* character = m_joltCharacter;
    JPH::Vec3 up = character->GetUp();
    return XMFLOAT3(up.GetX(), up.GetY(), up.GetZ());
}

void CharacterController::SetUp(const XMFLOAT3& up)
{
    if (!m_joltCharacter)
        return;

    auto* character = m_joltCharacter;
    character->SetUp(JPH::Vec3(up.x, up.y, up.z));
}
