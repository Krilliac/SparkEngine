#include "../Core/Platform.h"
/**
 * @file PhysicsBodyImpl.cpp
 * @brief PhysicsBody and PhysicsConstraint method implementations
 * @author Spark Engine Team
 * @date 2025
 *
 * Extracted from PhysicsSystem.cpp. Contains all PhysicsBody methods
 * (transform, velocity, forces, mass/material, state, collision groups,
 * interpolation, console helpers) and all PhysicsConstraint methods.
 */

#include "PhysicsBody.h"
#include "PhysicsSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Constraints/Constraint.h>

JPH_SUPPRESS_WARNINGS

#include <sstream>

using namespace DirectX;

// Forward declaration — defined in the engine's global scope
extern ::PhysicsSystem* g_physicsSystem;

// Helper to get the Jolt BodyInterface
static JPH::BodyInterface& GetBodyInterface()
{
    return g_physicsSystem->GetJoltSystem()->GetBodyInterface();
}

static bool HasValidJoltBody(uint32_t id)
{
    auto* sys = g_physicsSystem ? g_physicsSystem->GetJoltSystem() : nullptr;
    if (!sys)
        return false;
    return sys->GetBodyInterface().IsAdded(JPH::BodyID(id));
}

// ============================================================================
// PHYSICS BODY IMPLEMENTATION
// ============================================================================

PhysicsBody::PhysicsBody(const PhysicsBodyDesc& desc, uint32_t joltBodyID) : m_desc(desc), m_joltBodyID(joltBodyID)
{
    // Initialize interpolation state from the initial position
    m_previousPosition = desc.position;
    m_currentPosition = desc.position;
    XMVECTOR quat = XMQuaternionRotationRollPitchYaw(desc.rotation.x, desc.rotation.y, desc.rotation.z);
    XMStoreFloat4(&m_previousRotation, quat);
    XMStoreFloat4(&m_currentRotation, quat);
}

PhysicsBody::~PhysicsBody()
{
    // Body removal from Jolt is handled by PhysicsSystem::RemoveBody()
    // Do not destroy the Jolt body here — PhysicsSystem owns the lifecycle.
}

XMFLOAT3 PhysicsBody::GetPosition() const
{
    if (!HasValidJoltBody(m_joltBodyID))
        return m_desc.position;

    JPH::RVec3 pos = GetBodyInterface().GetPosition(JPH::BodyID(m_joltBodyID));
    return XMFLOAT3(static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()));
}

void PhysicsBody::SetPosition(const XMFLOAT3& position)
{
    m_desc.position = position;
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    GetBodyInterface().SetPosition(JPH::BodyID(m_joltBodyID), JPH::RVec3(position.x, position.y, position.z),
                                   JPH::EActivation::Activate);
}

XMFLOAT3 PhysicsBody::GetRotation() const
{
    if (!HasValidJoltBody(m_joltBodyID))
        return m_desc.rotation;

    JPH::Quat rot = GetBodyInterface().GetRotation(JPH::BodyID(m_joltBodyID));

    // Convert quaternion to Euler angles (roll, yaw, pitch)
    JPH::Vec3 euler = rot.GetEulerAngles();
    return XMFLOAT3(euler.GetX(), euler.GetY(), euler.GetZ());
}

void PhysicsBody::SetRotation(const XMFLOAT3& rotation)
{
    m_desc.rotation = rotation;
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    JPH::Quat quat = JPH::Quat::sEulerAngles(JPH::Vec3(rotation.x, rotation.y, rotation.z));
    GetBodyInterface().SetRotation(JPH::BodyID(m_joltBodyID), quat, JPH::EActivation::Activate);
}

XMMATRIX PhysicsBody::GetTransform() const
{
    if (!HasValidJoltBody(m_joltBodyID))
    {
        XMMATRIX translation = XMMatrixTranslation(m_desc.position.x, m_desc.position.y, m_desc.position.z);
        XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_desc.rotation.x, m_desc.rotation.y, m_desc.rotation.z);
        return rotation * translation;
    }

    JPH::RMat44 transform = GetBodyInterface().GetWorldTransform(JPH::BodyID(m_joltBodyID));

    // Convert Jolt's column-major 4x4 matrix to DirectX XMMATRIX (row-major)
    JPH::Vec4 col0 = transform.GetColumn4(0);
    JPH::Vec4 col1 = transform.GetColumn4(1);
    JPH::Vec4 col2 = transform.GetColumn4(2);
    JPH::Vec3 translation = transform.GetTranslation();

    return XMMATRIX(col0.GetX(), col1.GetX(), col2.GetX(), 0.0f, col0.GetY(), col1.GetY(), col2.GetY(), 0.0f,
                    col0.GetZ(), col1.GetZ(), col2.GetZ(), 0.0f, static_cast<float>(translation.GetX()),
                    static_cast<float>(translation.GetY()), static_cast<float>(translation.GetZ()), 1.0f);
}

void PhysicsBody::SetTransform(const XMMATRIX& transform)
{
    XMVECTOR scale, rotation, translation;
    XMMatrixDecompose(&scale, &rotation, &translation, transform);

    XMStoreFloat3(&m_desc.position, translation);

    XMFLOAT4 rotQuat;
    XMStoreFloat4(&rotQuat, rotation);
    m_desc.rotation = {rotQuat.x, rotQuat.y, rotQuat.z};

    if (!HasValidJoltBody(m_joltBodyID))
        return;

    auto& bi = GetBodyInterface();
    bi.SetPositionAndRotation(JPH::BodyID(m_joltBodyID),
                              JPH::RVec3(m_desc.position.x, m_desc.position.y, m_desc.position.z),
                              JPH::Quat(rotQuat.x, rotQuat.y, rotQuat.z, rotQuat.w), JPH::EActivation::Activate);
}

// ============================================================================
// PHYSICS BODY INTERPOLATION
// ============================================================================

XMFLOAT3 PhysicsBody::GetInterpolatedPosition(float alpha) const
{
    XMVECTOR prev = XMLoadFloat3(&m_previousPosition);
    XMVECTOR curr = XMLoadFloat3(&m_currentPosition);
    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVectorLerp(prev, curr, alpha));
    return result;
}

XMMATRIX PhysicsBody::GetInterpolatedTransform(float alpha) const
{
    XMVECTOR prevPos = XMLoadFloat3(&m_previousPosition);
    XMVECTOR currPos = XMLoadFloat3(&m_currentPosition);
    XMVECTOR interpPos = XMVectorLerp(prevPos, currPos, alpha);

    XMVECTOR prevRot = XMLoadFloat4(&m_previousRotation);
    XMVECTOR currRot = XMLoadFloat4(&m_currentRotation);
    XMVECTOR interpRot = XMQuaternionSlerp(prevRot, currRot, alpha);

    XMMATRIX rotMatrix = XMMatrixRotationQuaternion(interpRot);
    XMFLOAT3 pos;
    XMStoreFloat3(&pos, interpPos);
    XMMATRIX transMatrix = XMMatrixTranslation(pos.x, pos.y, pos.z);

    return rotMatrix * transMatrix;
}

void PhysicsBody::StoreCurrentState()
{
    m_previousPosition = m_currentPosition;
    m_previousRotation = m_currentRotation;
}

void PhysicsBody::UpdateCurrentState()
{
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    auto& bi = GetBodyInterface();
    JPH::RVec3 pos = bi.GetPosition(JPH::BodyID(m_joltBodyID));
    m_currentPosition =
        XMFLOAT3(static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()));

    JPH::Quat rot = bi.GetRotation(JPH::BodyID(m_joltBodyID));
    m_currentRotation = XMFLOAT4(rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW());
}

XMFLOAT3 PhysicsBody::GetLinearVelocity() const
{
    if (!HasValidJoltBody(m_joltBodyID))
        return m_desc.linearVelocity;

    JPH::Vec3 vel = GetBodyInterface().GetLinearVelocity(JPH::BodyID(m_joltBodyID));
    return XMFLOAT3(vel.GetX(), vel.GetY(), vel.GetZ());
}

void PhysicsBody::SetLinearVelocity(const XMFLOAT3& velocity)
{
    m_desc.linearVelocity = velocity;
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    GetBodyInterface().SetLinearVelocity(JPH::BodyID(m_joltBodyID), JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

XMFLOAT3 PhysicsBody::GetAngularVelocity() const
{
    if (!HasValidJoltBody(m_joltBodyID))
        return m_desc.angularVelocity;

    JPH::Vec3 vel = GetBodyInterface().GetAngularVelocity(JPH::BodyID(m_joltBodyID));
    return XMFLOAT3(vel.GetX(), vel.GetY(), vel.GetZ());
}

void PhysicsBody::SetAngularVelocity(const XMFLOAT3& velocity)
{
    m_desc.angularVelocity = velocity;
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    GetBodyInterface().SetAngularVelocity(JPH::BodyID(m_joltBodyID), JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

void PhysicsBody::ApplyForce(const XMFLOAT3& force, const XMFLOAT3& relativePos)
{
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    auto& bi = GetBodyInterface();
    if (relativePos.x == 0 && relativePos.y == 0 && relativePos.z == 0)
    {
        bi.AddForce(JPH::BodyID(m_joltBodyID), JPH::Vec3(force.x, force.y, force.z));
    }
    else
    {
        // Get body position and add relative offset for world-space application point
        JPH::RVec3 bodyPos = bi.GetPosition(JPH::BodyID(m_joltBodyID));
        JPH::RVec3 worldPoint = bodyPos + JPH::Vec3(relativePos.x, relativePos.y, relativePos.z);
        bi.AddForce(JPH::BodyID(m_joltBodyID), JPH::Vec3(force.x, force.y, force.z), worldPoint);
    }
}

void PhysicsBody::ApplyImpulse(const XMFLOAT3& impulse, const XMFLOAT3& relativePos)
{
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    auto& bi = GetBodyInterface();
    if (relativePos.x == 0 && relativePos.y == 0 && relativePos.z == 0)
    {
        bi.AddImpulse(JPH::BodyID(m_joltBodyID), JPH::Vec3(impulse.x, impulse.y, impulse.z));
    }
    else
    {
        JPH::RVec3 bodyPos = bi.GetPosition(JPH::BodyID(m_joltBodyID));
        JPH::RVec3 worldPoint = bodyPos + JPH::Vec3(relativePos.x, relativePos.y, relativePos.z);
        bi.AddImpulse(JPH::BodyID(m_joltBodyID), JPH::Vec3(impulse.x, impulse.y, impulse.z), worldPoint);
    }
}

void PhysicsBody::ApplyTorque(const XMFLOAT3& torque)
{
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    GetBodyInterface().AddTorque(JPH::BodyID(m_joltBodyID), JPH::Vec3(torque.x, torque.y, torque.z));
}

void PhysicsBody::ApplyTorqueImpulse(const XMFLOAT3& torque)
{
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    GetBodyInterface().AddAngularImpulse(JPH::BodyID(m_joltBodyID), JPH::Vec3(torque.x, torque.y, torque.z));
}

float PhysicsBody::GetMass() const
{
    if (!HasValidJoltBody(m_joltBodyID))
        return m_desc.mass;

    auto* sys = g_physicsSystem->GetJoltSystem();
    JPH::BodyID bodyID(m_joltBodyID);
    const JPH::Body* body = sys->GetBodyLockInterface().TryGetBody(bodyID);
    if (!body || !body->IsDynamic())
        return 0.0f;
    float invMass = body->GetMotionProperties()->GetInverseMass();
    if (invMass == 0.0f)
        return 0.0f;
    return 1.0f / invMass;
}

void PhysicsBody::SetMass(float mass)
{
    m_desc.mass = mass;
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    auto* sys = g_physicsSystem->GetJoltSystem();
    JPH::BodyID bodyID(m_joltBodyID);
    JPH::Body* body = const_cast<JPH::Body*>(sys->GetBodyLockInterface().TryGetBody(bodyID));
    if (body && body->IsDynamic() && mass > 0.0f)
    {
        body->GetMotionProperties()->SetInverseMass(1.0f / mass);
    }
}

void PhysicsBody::SetMaterial(const PhysicsMaterial& material)
{
    m_desc.material = material;
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    auto& bi = GetBodyInterface();
    JPH::BodyID bodyID(m_joltBodyID);
    bi.SetFriction(bodyID, material.friction);
    bi.SetRestitution(bodyID, material.restitution);

    auto* sys = g_physicsSystem->GetJoltSystem();
    JPH::Body* body = const_cast<JPH::Body*>(sys->GetBodyLockInterface().TryGetBody(bodyID));
    if (body && body->IsDynamic())
    {
        body->GetMotionProperties()->SetLinearDamping(material.linearDamping);
        body->GetMotionProperties()->SetAngularDamping(material.angularDamping);
    }
}

void PhysicsBody::SetActive(bool active)
{
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    auto& bi = GetBodyInterface();
    if (active)
    {
        bi.ActivateBody(JPH::BodyID(m_joltBodyID));
    }
    else
    {
        bi.DeactivateBody(JPH::BodyID(m_joltBodyID));
    }
}

bool PhysicsBody::IsActive() const
{
    if (!HasValidJoltBody(m_joltBodyID))
        return false;
    return GetBodyInterface().IsActive(JPH::BodyID(m_joltBodyID));
}

void PhysicsBody::SetKinematic(bool kinematic)
{
    m_desc.isKinematic = kinematic;
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    auto& bi = GetBodyInterface();
    if (kinematic)
    {
        bi.SetMotionType(JPH::BodyID(m_joltBodyID), JPH::EMotionType::Kinematic, JPH::EActivation::Activate);
    }
    else
    {
        bi.SetMotionType(JPH::BodyID(m_joltBodyID), JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
    }
}

bool PhysicsBody::IsKinematic() const
{
    if (!HasValidJoltBody(m_joltBodyID))
        return m_desc.isKinematic;
    return GetBodyInterface().GetMotionType(JPH::BodyID(m_joltBodyID)) == JPH::EMotionType::Kinematic;
}

void PhysicsBody::SetTrigger(bool trigger)
{
    m_desc.isTrigger = trigger;
    if (!HasValidJoltBody(m_joltBodyID))
        return;

    auto& bi = GetBodyInterface();
    if (trigger)
    {
        bi.SetObjectLayer(JPH::BodyID(m_joltBodyID), 2); // TRIGGER layer
    }
    else
    {
        // Restore based on motion type
        JPH::EMotionType mt = bi.GetMotionType(JPH::BodyID(m_joltBodyID));
        bi.SetObjectLayer(JPH::BodyID(m_joltBodyID), mt == JPH::EMotionType::Static ? 0 : 1);
    }
}

bool PhysicsBody::IsTrigger() const
{
    if (!HasValidJoltBody(m_joltBodyID))
        return m_desc.isTrigger;
    return m_desc.isTrigger;
}

void PhysicsBody::SetCollisionGroup(uint16_t group)
{
    m_collisionGroup = group;
    ApplyCollisionFilter();
}

void PhysicsBody::SetCollisionMask(uint16_t mask)
{
    m_collisionMask = mask;
    ApplyCollisionFilter();
}

void PhysicsBody::ApplyCollisionFilter()
{
    // Jolt uses ObjectLayer + GroupFilter for collision filtering.
    // The group/mask bitmask approach is stored locally and can be used
    // via a custom GroupFilter if needed. For the default setup, object
    // layers handle basic filtering.
}

std::string PhysicsBody::GetInfo() const
{
    std::stringstream ss;
    ss << "Physics Body: " << m_desc.name << "\n";
    ss << "Type: " << PhysicsBodyTypeToString(m_desc.type) << "\n";
    auto pos = GetPosition();
    ss << "Position: (" << pos.x << ", " << pos.y << ", " << pos.z << ")\n";
    ss << "Mass: " << GetMass() << "\n";
    ss << "Active: " << (IsActive() ? "Yes" : "No") << "\n";
    ss << "Kinematic: " << (IsKinematic() ? "Yes" : "No") << "\n";
    ss << "Trigger: " << (IsTrigger() ? "Yes" : "No") << "\n";
    return ss.str();
}

void PhysicsBody::Console_SetProperty(const std::string& property, float value)
{
    if (property == "mass")
    {
        SetMass(value);
    }
    else if (property == "friction")
    {
        m_desc.material.friction = value;
        if (HasValidJoltBody(m_joltBodyID))
            GetBodyInterface().SetFriction(JPH::BodyID(m_joltBodyID), value);
    }
    else if (property == "restitution")
    {
        m_desc.material.restitution = value;
        if (HasValidJoltBody(m_joltBodyID))
            GetBodyInterface().SetRestitution(JPH::BodyID(m_joltBodyID), value);
    }
}

void PhysicsBody::Console_ApplyForce(float x, float y, float z)
{
    ApplyForce({x, y, z});
}

// ============================================================================
// PHYSICS CONSTRAINT IMPLEMENTATION
// ============================================================================

PhysicsConstraint::PhysicsConstraint(ConstraintType type, JPH::Constraint* joltConstraint)
    : m_type(type), m_joltConstraint(joltConstraint)
{
}

PhysicsConstraint::~PhysicsConstraint()
{
    // WARNING: The constraint MUST be removed from the physics system via
    // PhysicsSystem::RemoveConstraint() BEFORE this destructor runs.
    // Jolt constraints are ref-counted so we just release our reference.
    m_joltConstraint = nullptr;
}

void PhysicsConstraint::SetEnabled(bool enabled)
{
    if (!m_joltConstraint)
        return;
    m_joltConstraint->SetEnabled(enabled);
}

bool PhysicsConstraint::IsEnabled() const
{
    if (!m_joltConstraint)
        return false;
    return m_joltConstraint->GetEnabled();
}

void PhysicsConstraint::SetBreakingThreshold(float threshold)
{
    // Store the threshold for per-frame force checking in PhysicsSystem::Update().
    // When the constraint force exceeds this value, the constraint is removed.
    m_breakingThreshold = threshold;
}

float PhysicsConstraint::GetBreakingThreshold() const
{
    return m_breakingThreshold;
}
