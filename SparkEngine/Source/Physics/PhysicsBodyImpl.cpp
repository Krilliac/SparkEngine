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

#include <btBulletDynamicsCommon.h>

#include <sstream>

using namespace DirectX;

// ============================================================================
// PHYSICS BODY IMPLEMENTATION
// ============================================================================

PhysicsBody::PhysicsBody(const PhysicsBodyDesc& desc, btRigidBody* bulletBody) : m_desc(desc), m_bulletBody(bulletBody)
{
    // Initialize interpolation state from the initial position
    m_previousPosition = desc.position;
    m_currentPosition = desc.position;
    // Convert initial Euler rotation to quaternion for interpolation
    XMVECTOR quat = XMQuaternionRotationRollPitchYaw(desc.rotation.x, desc.rotation.y, desc.rotation.z);
    XMStoreFloat4(&m_previousRotation, quat);
    XMStoreFloat4(&m_currentRotation, quat);
}

PhysicsBody::~PhysicsBody()
{
    if (m_bulletBody)
    {
        delete m_bulletBody->getMotionState();
        // NOTE: Do not delete the collision shape here — shapes are cached and
        // shared via PhysicsSystem::m_shapeCache. They are cleaned up when the
        // PhysicsSystem is shut down.
        delete m_bulletBody;
        m_bulletBody = nullptr;
    }
}

XMFLOAT3 PhysicsBody::GetPosition() const
{
    if (!m_bulletBody)
        return m_desc.position;

    btTransform transform;
    if (m_bulletBody->getMotionState())
    {
        m_bulletBody->getMotionState()->getWorldTransform(transform);
    }
    else
    {
        transform = m_bulletBody->getWorldTransform();
    }

    const btVector3& origin = transform.getOrigin();
    return XMFLOAT3(origin.getX(), origin.getY(), origin.getZ());
}

void PhysicsBody::SetPosition(const XMFLOAT3& position)
{
    m_desc.position = position;
    if (!m_bulletBody)
        return;

    btTransform transform = m_bulletBody->getWorldTransform();
    transform.setOrigin(btVector3(position.x, position.y, position.z));
    m_bulletBody->setWorldTransform(transform);

    if (m_bulletBody->getMotionState())
    {
        m_bulletBody->getMotionState()->setWorldTransform(transform);
    }

    m_bulletBody->activate();
}

XMFLOAT3 PhysicsBody::GetRotation() const
{
    if (!m_bulletBody)
        return m_desc.rotation;

    btTransform transform;
    if (m_bulletBody->getMotionState())
    {
        m_bulletBody->getMotionState()->getWorldTransform(transform);
    }
    else
    {
        transform = m_bulletBody->getWorldTransform();
    }

    btQuaternion quat = transform.getRotation();
    btScalar yaw, pitch, roll;
    btMatrix3x3(quat).getEulerYPR(yaw, pitch, roll);
    return XMFLOAT3(roll, yaw, pitch);
}

void PhysicsBody::SetRotation(const XMFLOAT3& rotation)
{
    m_desc.rotation = rotation;
    if (!m_bulletBody)
        return;

    btTransform transform = m_bulletBody->getWorldTransform();
    btQuaternion quat;
    quat.setEulerZYX(rotation.z, rotation.y, rotation.x);
    transform.setRotation(quat);
    m_bulletBody->setWorldTransform(transform);

    if (m_bulletBody->getMotionState())
    {
        m_bulletBody->getMotionState()->setWorldTransform(transform);
    }

    m_bulletBody->activate();
}

XMMATRIX PhysicsBody::GetTransform() const
{
    if (!m_bulletBody)
    {
        XMMATRIX translation = XMMatrixTranslation(m_desc.position.x, m_desc.position.y, m_desc.position.z);
        XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_desc.rotation.x, m_desc.rotation.y, m_desc.rotation.z);
        return rotation * translation;
    }

    btTransform btTrans;
    if (m_bulletBody->getMotionState())
    {
        m_bulletBody->getMotionState()->getWorldTransform(btTrans);
    }
    else
    {
        btTrans = m_bulletBody->getWorldTransform();
    }

    float m[16];
    btTrans.getOpenGLMatrix(m);

    return XMMATRIX(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14],
                    m[15]);
}

void PhysicsBody::SetTransform(const XMMATRIX& transform)
{
    XMVECTOR scale, rotation, translation;
    XMMatrixDecompose(&scale, &rotation, &translation, transform);

    XMStoreFloat3(&m_desc.position, translation);

    XMFLOAT4 rotQuat;
    XMStoreFloat4(&rotQuat, rotation);
    m_desc.rotation = {rotQuat.x, rotQuat.y, rotQuat.z};

    if (!m_bulletBody)
        return;

    btTransform btTrans;
    btTrans.setOrigin(btVector3(m_desc.position.x, m_desc.position.y, m_desc.position.z));
    btTrans.setRotation(btQuaternion(rotQuat.x, rotQuat.y, rotQuat.z, rotQuat.w));
    m_bulletBody->setWorldTransform(btTrans);

    if (m_bulletBody->getMotionState())
    {
        m_bulletBody->getMotionState()->setWorldTransform(btTrans);
    }

    m_bulletBody->activate();
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
    // Lerp position
    XMVECTOR prevPos = XMLoadFloat3(&m_previousPosition);
    XMVECTOR currPos = XMLoadFloat3(&m_currentPosition);
    XMVECTOR interpPos = XMVectorLerp(prevPos, currPos, alpha);

    // Slerp rotation
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
    if (!m_bulletBody)
        return;

    btTransform transform;
    if (m_bulletBody->getMotionState())
    {
        m_bulletBody->getMotionState()->getWorldTransform(transform);
    }
    else
    {
        transform = m_bulletBody->getWorldTransform();
    }

    const btVector3& origin = transform.getOrigin();
    m_currentPosition = XMFLOAT3(origin.getX(), origin.getY(), origin.getZ());

    const btQuaternion& rot = transform.getRotation();
    m_currentRotation = XMFLOAT4(rot.getX(), rot.getY(), rot.getZ(), rot.getW());
}

XMFLOAT3 PhysicsBody::GetLinearVelocity() const
{
    if (!m_bulletBody)
        return m_desc.linearVelocity;

    const btVector3& vel = m_bulletBody->getLinearVelocity();
    return XMFLOAT3(vel.getX(), vel.getY(), vel.getZ());
}

void PhysicsBody::SetLinearVelocity(const XMFLOAT3& velocity)
{
    m_desc.linearVelocity = velocity;
    if (!m_bulletBody)
        return;

    m_bulletBody->setLinearVelocity(btVector3(velocity.x, velocity.y, velocity.z));
    m_bulletBody->activate();
}

XMFLOAT3 PhysicsBody::GetAngularVelocity() const
{
    if (!m_bulletBody)
        return m_desc.angularVelocity;

    const btVector3& vel = m_bulletBody->getAngularVelocity();
    return XMFLOAT3(vel.getX(), vel.getY(), vel.getZ());
}

void PhysicsBody::SetAngularVelocity(const XMFLOAT3& velocity)
{
    m_desc.angularVelocity = velocity;
    if (!m_bulletBody)
        return;

    m_bulletBody->setAngularVelocity(btVector3(velocity.x, velocity.y, velocity.z));
    m_bulletBody->activate();
}

void PhysicsBody::ApplyForce(const XMFLOAT3& force, const XMFLOAT3& relativePos)
{
    if (!m_bulletBody)
        return;

    m_bulletBody->applyForce(btVector3(force.x, force.y, force.z),
                             btVector3(relativePos.x, relativePos.y, relativePos.z));
    m_bulletBody->activate();
}

void PhysicsBody::ApplyImpulse(const XMFLOAT3& impulse, const XMFLOAT3& relativePos)
{
    if (!m_bulletBody)
        return;

    m_bulletBody->applyImpulse(btVector3(impulse.x, impulse.y, impulse.z),
                               btVector3(relativePos.x, relativePos.y, relativePos.z));
    m_bulletBody->activate();
}

void PhysicsBody::ApplyTorque(const XMFLOAT3& torque)
{
    if (!m_bulletBody)
        return;

    m_bulletBody->applyTorque(btVector3(torque.x, torque.y, torque.z));
    m_bulletBody->activate();
}

void PhysicsBody::ApplyTorqueImpulse(const XMFLOAT3& torque)
{
    if (!m_bulletBody)
        return;

    m_bulletBody->applyTorqueImpulse(btVector3(torque.x, torque.y, torque.z));
    m_bulletBody->activate();
}

float PhysicsBody::GetMass() const
{
    if (!m_bulletBody)
        return m_desc.mass;

    btScalar invMass = m_bulletBody->getInvMass();
    if (invMass == btScalar(0))
        return 0.0f;
    return 1.0f / invMass;
}

void PhysicsBody::SetMass(float mass)
{
    m_desc.mass = mass;
    if (!m_bulletBody)
        return;

    btVector3 localInertia(0, 0, 0);
    if (mass > 0.0f && m_bulletBody->getCollisionShape())
    {
        m_bulletBody->getCollisionShape()->calculateLocalInertia(mass, localInertia);
    }
    m_bulletBody->setMassProps(mass, localInertia);
    m_bulletBody->updateInertiaTensor();
    m_bulletBody->activate();
}

void PhysicsBody::SetMaterial(const PhysicsMaterial& material)
{
    m_desc.material = material;
    if (!m_bulletBody)
        return;

    m_bulletBody->setFriction(material.friction);
    m_bulletBody->setRestitution(material.restitution);
    m_bulletBody->setDamping(material.linearDamping, material.angularDamping);
}

void PhysicsBody::SetActive(bool active)
{
    if (!m_bulletBody)
        return;

    if (active)
    {
        m_bulletBody->setActivationState(ACTIVE_TAG);
    }
    else
    {
        m_bulletBody->setActivationState(WANTS_DEACTIVATION);
    }
}

bool PhysicsBody::IsActive() const
{
    if (!m_bulletBody)
        return false;
    return m_bulletBody->isActive();
}

void PhysicsBody::SetKinematic(bool kinematic)
{
    m_desc.isKinematic = kinematic;
    if (!m_bulletBody)
        return;

    if (kinematic)
    {
        m_bulletBody->setCollisionFlags(m_bulletBody->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
        m_bulletBody->setActivationState(DISABLE_DEACTIVATION);
    }
    else
    {
        m_bulletBody->setCollisionFlags(m_bulletBody->getCollisionFlags() & ~btCollisionObject::CF_KINEMATIC_OBJECT);
        m_bulletBody->setActivationState(ACTIVE_TAG);
    }
}

bool PhysicsBody::IsKinematic() const
{
    if (!m_bulletBody)
        return m_desc.isKinematic;
    return (m_bulletBody->getCollisionFlags() & btCollisionObject::CF_KINEMATIC_OBJECT) != 0;
}

void PhysicsBody::SetTrigger(bool trigger)
{
    m_desc.isTrigger = trigger;
    if (!m_bulletBody)
        return;

    if (trigger)
    {
        m_bulletBody->setCollisionFlags(m_bulletBody->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE);
    }
    else
    {
        m_bulletBody->setCollisionFlags(m_bulletBody->getCollisionFlags() & ~btCollisionObject::CF_NO_CONTACT_RESPONSE);
    }
}

bool PhysicsBody::IsTrigger() const
{
    if (!m_bulletBody)
        return m_desc.isTrigger;
    return (m_bulletBody->getCollisionFlags() & btCollisionObject::CF_NO_CONTACT_RESPONSE) != 0;
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
    if (!m_bulletBody)
        return;

    // Update Bullet's broadphase proxy with the new group/mask
    btBroadphaseProxy* proxy = m_bulletBody->getBroadphaseProxy();
    if (proxy)
    {
        proxy->m_collisionFilterGroup = m_collisionGroup;
        proxy->m_collisionFilterMask = m_collisionMask;
    }
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
        if (m_bulletBody)
            m_bulletBody->setFriction(value);
    }
    else if (property == "restitution")
    {
        m_desc.material.restitution = value;
        if (m_bulletBody)
            m_bulletBody->setRestitution(value);
    }
}

void PhysicsBody::Console_ApplyForce(float x, float y, float z)
{
    ApplyForce({x, y, z});
}

// ============================================================================
// PHYSICS CONSTRAINT IMPLEMENTATION
// ============================================================================

PhysicsConstraint::PhysicsConstraint(ConstraintType type, btTypedConstraint* bulletConstraint)
    : m_type(type), m_bulletConstraint(bulletConstraint)
{
}

PhysicsConstraint::~PhysicsConstraint()
{
    // WARNING: The constraint MUST be removed from the dynamics world via
    // PhysicsSystem::RemoveConstraint() BEFORE this destructor runs.
    // Deleting a constraint that is still registered in a btDynamicsWorld
    // causes use-after-free on the next simulation step.
    if (m_bulletConstraint)
    {
        delete m_bulletConstraint;
        m_bulletConstraint = nullptr;
    }
}

void PhysicsConstraint::SetEnabled(bool enabled)
{
    if (!m_bulletConstraint)
        return;
    m_bulletConstraint->setEnabled(enabled);
}

bool PhysicsConstraint::IsEnabled() const
{
    if (!m_bulletConstraint)
        return false;
    return m_bulletConstraint->isEnabled();
}

void PhysicsConstraint::SetBreakingThreshold(float threshold)
{
    if (!m_bulletConstraint)
        return;
    m_bulletConstraint->setBreakingImpulseThreshold(threshold);
}

float PhysicsConstraint::GetBreakingThreshold() const
{
    if (!m_bulletConstraint)
        return 0.0f;
    return m_bulletConstraint->getBreakingImpulseThreshold();
}
