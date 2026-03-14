#include "../Core/Platform.h"
/**
 * @file PhysicsSystem.cpp
 * @brief Implementation of complete physics integration system using Bullet Physics
 * @author Spark Engine Team
 * @date 2025
 */

#include "PhysicsSystem.h"
#include "Utils/Assert.h"
#include "../Utils/Hash.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#include <iostream>
#include <sstream>
#include <algorithm>

// Physics system is now accessed via EngineContext::Get()->GetPhysics()
#include <chrono>

using namespace DirectX;

// ============================================================================
// BULLET CONVERSION HELPERS
// ============================================================================

btVector3 PhysicsSystem::ToBullet(const XMFLOAT3& vec) const
{
    return btVector3(vec.x, vec.y, vec.z);
}

XMFLOAT3 PhysicsSystem::FromBullet(const btVector3& vec) const
{
    return XMFLOAT3(vec.getX(), vec.getY(), vec.getZ());
}

btQuaternion PhysicsSystem::ToBulletQuaternion(const XMFLOAT3& euler) const
{
    btQuaternion quat;
    quat.setEulerZYX(euler.z, euler.y, euler.x);
    return quat;
}

XMFLOAT3 PhysicsSystem::FromBullet(const btQuaternion& quat) const
{
    btScalar yaw, pitch, roll;
    btMatrix3x3(quat).getEulerYPR(yaw, pitch, roll);
    return XMFLOAT3(roll, yaw, pitch);
}

// ============================================================================
// COLLISION SHAPE CREATION
// ============================================================================

btCollisionShape* PhysicsSystem::CreateCollisionShape(const CollisionShapeDesc& desc)
{
    // Check shape cache first
    size_t hash = HashShape(desc);
    auto it = m_shapeCache.find(hash);
    if (it != m_shapeCache.end())
    {
        return it->second;
    }

    btCollisionShape* shape = nullptr;

    switch (desc.type)
    {
    case CollisionShapeType::Box:
        shape = CreateBoxShape(desc.dimensions);
        break;
    case CollisionShapeType::Sphere:
        shape = CreateSphereShape(desc.radius);
        break;
    case CollisionShapeType::Capsule:
        shape = CreateCapsuleShape(desc.radius, desc.height);
        break;
    case CollisionShapeType::Cylinder:
        shape = CreateCylinderShape(desc.radius, desc.height);
        break;
    case CollisionShapeType::Cone:
        shape = CreateConeShape(desc.radius, desc.height);
        break;
    case CollisionShapeType::Mesh:
        shape = CreateMeshShape(desc.vertices, desc.indices);
        break;
    case CollisionShapeType::ConvexHull:
        shape = CreateConvexHullShape(desc.vertices);
        break;
    default:
        // Default to box shape
        shape = CreateBoxShape(desc.dimensions);
        break;
    }

    if (shape)
    {
        m_shapeCache[hash] = shape;
    }

    return shape;
}

btCollisionShape* PhysicsSystem::CreateBoxShape(const XMFLOAT3& dimensions)
{
    return new btBoxShape(btVector3(dimensions.x / 2.0f, dimensions.y / 2.0f, dimensions.z / 2.0f));
}

btCollisionShape* PhysicsSystem::CreateSphereShape(float radius)
{
    return new btSphereShape(radius);
}

btCollisionShape* PhysicsSystem::CreateCapsuleShape(float radius, float height)
{
    return new btCapsuleShape(radius, height);
}

btCollisionShape* PhysicsSystem::CreateCylinderShape(float radius, float height)
{
    return new btCylinderShape(btVector3(radius, height / 2.0f, radius));
}

btCollisionShape* PhysicsSystem::CreateConeShape(float radius, float height)
{
    return new btConeShape(radius, height);
}

btCollisionShape* PhysicsSystem::CreateMeshShape(const std::vector<XMFLOAT3>& vertices,
                                                 const std::vector<uint32_t>& indices)
{
    if (vertices.empty() || indices.empty() || indices.size() % 3 != 0)
    {
        // Fall back to a unit box if mesh data is invalid
        return CreateBoxShape({1.0f, 1.0f, 1.0f});
    }

    btTriangleMesh* triMesh = new btTriangleMesh();

    const auto vertexCount = static_cast<uint32_t>(vertices.size());
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        // Bounds-check indices to prevent out-of-range access on corrupted data
        if (indices[i] >= vertexCount || indices[i + 1] >= vertexCount || indices[i + 2] >= vertexCount)
            continue;

        const XMFLOAT3& v0 = vertices[indices[i]];
        const XMFLOAT3& v1 = vertices[indices[i + 1]];
        const XMFLOAT3& v2 = vertices[indices[i + 2]];

        triMesh->addTriangle(btVector3(v0.x, v0.y, v0.z), btVector3(v1.x, v1.y, v1.z), btVector3(v2.x, v2.y, v2.z));
    }

    btBvhTriangleMeshShape* meshShape = new btBvhTriangleMeshShape(triMesh, true);
    return meshShape;
}

btCollisionShape* PhysicsSystem::CreateConvexHullShape(const std::vector<XMFLOAT3>& vertices)
{
    if (vertices.empty())
    {
        return CreateBoxShape({1.0f, 1.0f, 1.0f});
    }

    btConvexHullShape* convexShape = new btConvexHullShape();

    for (const auto& v : vertices)
    {
        convexShape->addPoint(btVector3(v.x, v.y, v.z), false);
    }

    convexShape->recalcLocalAabb();
    return convexShape;
}

size_t PhysicsSystem::HashShape(const CollisionShapeDesc& desc)
{
    size_t hash = std::hash<int>()(static_cast<int>(desc.type));

    Spark::CombineHash(hash, std::hash<float>()(desc.dimensions.x));
    Spark::CombineHash(hash, std::hash<float>()(desc.dimensions.y));
    Spark::CombineHash(hash, std::hash<float>()(desc.dimensions.z));
    Spark::CombineHash(hash, std::hash<float>()(desc.radius));
    Spark::CombineHash(hash, std::hash<float>()(desc.height));
    Spark::CombineHash(hash, std::hash<std::string>()(desc.meshPath));
    Spark::CombineHash(hash, std::hash<size_t>()(desc.vertices.size()));
    Spark::CombineHash(hash, std::hash<size_t>()(desc.indices.size()));

    return hash;
}

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
}

void PhysicsBody::SetCollisionMask(uint16_t mask)
{
    m_collisionMask = mask;
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

// ============================================================================
// PHYSICS SYSTEM IMPLEMENTATION
// ============================================================================

PhysicsSystem::PhysicsSystem()
    : m_dynamicsWorld(nullptr), m_collisionConfig(nullptr), m_dispatcher(nullptr), m_broadphase(nullptr),
      m_solver(nullptr), m_debugDrawer(nullptr)
{
}

PhysicsSystem::~PhysicsSystem()
{
    Shutdown();
}

HRESULT PhysicsSystem::Initialize()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_LOG_INFO(Spark::LogCategory::Physics, "PhysicsSystem initializing (Bullet Physics)");
    // Initialize default material
    m_defaultMaterial.friction = 0.5f;
    m_defaultMaterial.restitution = 0.1f;
    m_defaultMaterial.linearDamping = 0.1f;
    m_defaultMaterial.angularDamping = 0.1f;
    m_defaultMaterial.density = 1.0f;
    m_defaultMaterial.name = "Default";

    // Initialize metrics (value-initialization instead of memset)
    m_metrics = {};

    // Initialize Bullet Physics world
    m_collisionConfig = new btDefaultCollisionConfiguration();
    m_dispatcher = new btCollisionDispatcher(m_collisionConfig);
    m_broadphase = new btDbvtBroadphase();
    m_solver = new btSequentialImpulseConstraintSolver();
    m_dynamicsWorld = new btDiscreteDynamicsWorld(m_dispatcher, m_broadphase, m_solver, m_collisionConfig);
    m_dynamicsWorld->setGravity(btVector3(0, -9.8f, 0));

    // Enable ghost object pair callback for overlap tests
    m_broadphase->getOverlappingPairCache()->setInternalGhostPairCallback(new btGhostPairCallback());

    Spark::SimpleConsole::GetInstance().LogSuccess("PhysicsSystem initialized successfully (Bullet Physics)");
    return S_OK;
}

void PhysicsSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    // Guard against double shutdown (destructor also calls Shutdown)
    if (!m_dynamicsWorld && m_bodies.empty() && m_constraints.empty())
        return;

    SPARK_LOG_INFO(Spark::LogCategory::Physics, "PhysicsSystem shutting down");

    // Remove all constraints from the world first
    if (m_dynamicsWorld)
    {
        for (auto& constraint : m_constraints)
        {
            if (constraint && constraint->GetBulletConstraint())
            {
                m_dynamicsWorld->removeConstraint(constraint->GetBulletConstraint());
            }
        }
    }
    m_constraints.clear();

    // Remove all bodies from the world
    if (m_dynamicsWorld)
    {
        for (auto& body : m_bodies)
        {
            if (body && body->GetBulletBody())
            {
                m_dynamicsWorld->removeRigidBody(body->GetBulletBody());
            }
        }
    }
    m_bodies.clear();
    m_namedBodies.clear();

    // Delete cached collision shapes
    for (auto& [hash, shape] : m_shapeCache)
    {
        delete shape;
    }
    m_shapeCache.clear();

    // Delete Bullet world components in reverse order
    delete m_dynamicsWorld;
    m_dynamicsWorld = nullptr;

    delete m_solver;
    m_solver = nullptr;

    delete m_broadphase;
    m_broadphase = nullptr;

    delete m_dispatcher;
    m_dispatcher = nullptr;

    delete m_collisionConfig;
    m_collisionConfig = nullptr;

    Spark::SimpleConsole::GetInstance().LogInfo("PhysicsSystem shutdown complete");
}

void PhysicsSystem::Update(float deltaTime)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_WARN_IF(Spark::LogCategory::Physics, deltaTime < 0.0f,
                  "PhysicsSystem::Update called with negative deltaTime");
    if (m_paused || deltaTime <= 0.0f)
    {
        return;
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    if (m_dynamicsWorld)
    {
        if (m_interpolationEnabled)
        {
            // Fixed-timestep accumulator with interpolation
            m_accumulator += deltaTime;

            while (m_accumulator >= m_timeStep)
            {
                // Snapshot current state as previous before stepping
                for (auto& body : m_bodies)
                {
                    if (body)
                    {
                        body->StoreCurrentState();
                    }
                }

                m_dynamicsWorld->stepSimulation(m_timeStep, 1, m_timeStep);

                // Read back new state from Bullet
                for (auto& body : m_bodies)
                {
                    if (body)
                    {
                        body->UpdateCurrentState();
                    }
                }

                m_accumulator -= m_timeStep;
            }

            m_interpolationAlpha = (m_timeStep > 0.0f) ? m_accumulator / m_timeStep : 1.0f;
        }
        else
        {
            // Legacy behavior: pass deltaTime directly to Bullet
            m_dynamicsWorld->stepSimulation(deltaTime, m_maxSubsteps, m_timeStep);
        }
    }

    ProcessCollisions();

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.simulationTime = duration.count() / 1000.0f;
    }

    UpdateMetrics();
}

void PhysicsSystem::UpdateMetrics()
{
    if (!m_dynamicsWorld)
        return;

    std::lock_guard<std::mutex> lock(m_metricsMutex);

    m_metrics.totalRigidBodies = static_cast<uint32_t>(m_bodies.size());
    m_metrics.activeConstraints = static_cast<uint32_t>(m_constraints.size());
    m_metrics.timeStep = m_timeStep;
    m_metrics.debugDrawEnabled = m_debugDrawEnabled;

    // Count active rigid bodies
    m_metrics.activeRigidBodies = static_cast<uint32_t>(
        std::count_if(m_bodies.begin(), m_bodies.end(), [](const auto& body) { return body && body->IsActive(); }));

    // Collision pairs from dispatcher
    if (m_dispatcher)
    {
        m_metrics.collisionPairs = static_cast<uint32_t>(m_dispatcher->getNumManifolds());
    }

    // Broadphase proxies
    if (m_broadphase)
    {
        m_metrics.broadphaseProxies =
            static_cast<uint32_t>(m_broadphase->getOverlappingPairCache()->getNumOverlappingPairs());
    }

    m_metrics.substeps = static_cast<uint32_t>(m_maxSubsteps);
}

void PhysicsSystem::ProcessCollisions()
{
    if (!m_dynamicsWorld || !m_dispatcher)
        return;

    // Build the set of trigger pairs active this frame
    std::vector<std::pair<PhysicsBody*, PhysicsBody*>> currentTriggerPairs;

    int numManifolds = m_dispatcher->getNumManifolds();
    for (int i = 0; i < numManifolds; i++)
    {
        btPersistentManifold* manifold = m_dispatcher->getManifoldByIndexInternal(i);
        if (!manifold)
            continue;

        const btCollisionObject* objA = manifold->getBody0();
        const btCollisionObject* objB = manifold->getBody1();

        // Retrieve PhysicsBody wrappers via Bullet user pointer (O(1) instead of O(n) search)
        auto* bodyA = static_cast<PhysicsBody*>(objA->getUserPointer());
        auto* bodyB = static_cast<PhysicsBody*>(objB->getUserPointer());

        // Check for trigger callbacks
        bool isTrigger = false;
        if (bodyA && bodyA->IsTrigger())
            isTrigger = true;
        if (bodyB && bodyB->IsTrigger())
            isTrigger = true;

        int numContacts = manifold->getNumContacts();

        if (isTrigger && bodyA && bodyB && numContacts > 0)
        {
            // Canonical ordering to ensure consistent pair identity
            PhysicsBody* first = (bodyA < bodyB) ? bodyA : bodyB;
            PhysicsBody* second = (bodyA < bodyB) ? bodyB : bodyA;
            currentTriggerPairs.push_back({first, second});

            // Check if this pair is new (trigger enter)
            if (m_triggerCallback)
            {
                bool wasActive = false;
                for (const auto& prev : m_activeTriggerPairs)
                {
                    if (prev.first == first && prev.second == second)
                    {
                        wasActive = true;
                        break;
                    }
                }
                if (!wasActive)
                {
                    m_triggerCallback(bodyA, bodyB, true);
                }
            }
        }

        // Fire collision callbacks for each contact point
        if (m_collisionCallback && !isTrigger)
        {
            for (int j = 0; j < numContacts; j++)
            {
                btManifoldPoint& pt = manifold->getContactPoint(j);
                if (pt.getDistance() < 0.0f)
                {
                    ContactInfo info;
                    info.bodyA = bodyA;
                    info.bodyB = bodyB;
                    info.contactPoint = FromBullet(pt.getPositionWorldOnB());
                    info.contactNormal = FromBullet(pt.m_normalWorldOnB);
                    info.penetrationDepth = -pt.getDistance();
                    info.appliedImpulse = pt.getAppliedImpulse();

                    m_collisionCallback(info);
                }
            }
        }
    }

    // Detect trigger exit events: pairs that were active last frame but not this frame
    if (m_triggerCallback)
    {
        for (const auto& prev : m_activeTriggerPairs)
        {
            bool stillActive = false;
            for (const auto& curr : currentTriggerPairs)
            {
                if (curr.first == prev.first && curr.second == prev.second)
                {
                    stillActive = true;
                    break;
                }
            }
            if (!stillActive)
            {
                m_triggerCallback(prev.first, prev.second, false);
            }
        }
    }

    // Swap the current pairs into the tracking set for the next frame
    m_activeTriggerPairs = std::move(currentTriggerPairs);
}

void PhysicsSystem::SetGravity(const XMFLOAT3& gravity)
{
    if (!m_dynamicsWorld)
        return;
    m_dynamicsWorld->setGravity(ToBullet(gravity));
}

XMFLOAT3 PhysicsSystem::GetGravity() const
{
    if (!m_dynamicsWorld)
        return {0.0f, -9.8f, 0.0f};
    return FromBullet(m_dynamicsWorld->getGravity());
}

std::shared_ptr<PhysicsBody> PhysicsSystem::CreateBody(const PhysicsBodyDesc& desc)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Physics, m_dynamicsWorld, nullptr);

    // Create collision shape
    btCollisionShape* shape = CreateCollisionShape(desc.shape);
    if (!shape)
        return nullptr;

    // Determine mass (0 for static and kinematic bodies)
    float mass = desc.mass;
    if (desc.type == PhysicsBodyType::Static || desc.isKinematic)
    {
        mass = 0.0f;
    }

    // Calculate local inertia
    btVector3 localInertia(0, 0, 0);
    if (mass > 0.0f)
    {
        shape->calculateLocalInertia(mass, localInertia);
    }

    // Create initial transform
    btTransform startTransform;
    startTransform.setIdentity();
    startTransform.setOrigin(ToBullet(desc.position));
    startTransform.setRotation(ToBulletQuaternion(desc.rotation));

    // Create motion state
    btDefaultMotionState* motionState = new btDefaultMotionState(startTransform);

    // Create rigid body construction info
    btRigidBody::btRigidBodyConstructionInfo rbInfo(mass, motionState, shape, localInertia);
    rbInfo.m_friction = desc.material.friction;
    rbInfo.m_restitution = desc.material.restitution;
    rbInfo.m_linearDamping = desc.material.linearDamping;
    rbInfo.m_angularDamping = desc.material.angularDamping;

    // Create the Bullet rigid body
    btRigidBody* bulletBody = new btRigidBody(rbInfo);

    // Set kinematic flags
    if (desc.isKinematic)
    {
        bulletBody->setCollisionFlags(bulletBody->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
        bulletBody->setActivationState(DISABLE_DEACTIVATION);
    }

    // Set trigger flags
    if (desc.isTrigger)
    {
        bulletBody->setCollisionFlags(bulletBody->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE);
    }

    // Set initial velocities
    if (mass > 0.0f)
    {
        bulletBody->setLinearVelocity(ToBullet(desc.linearVelocity));
        bulletBody->setAngularVelocity(ToBullet(desc.angularVelocity));
    }

    // Add to dynamics world with collision filtering
    uint16_t group = 1;
    uint16_t mask = 0xFFFF;
    m_dynamicsWorld->addRigidBody(bulletBody, group, mask);

    // Create the PhysicsBody wrapper
    auto body = std::make_shared<PhysicsBody>(desc, bulletBody);

    // Store user data pointer on the bullet body for lookup during collision
    bulletBody->setUserPointer(body.get());

    m_bodies.push_back(body);

    if (!desc.name.empty())
    {
        m_namedBodies[desc.name] = body;
    }

    Spark::SimpleConsole::GetInstance().LogInfo("Created physics body: " + desc.name);
    return body;
}

void PhysicsSystem::RemoveBody(std::shared_ptr<PhysicsBody> body)
{
    if (!body)
        return;

    // Remove from Bullet world
    if (m_dynamicsWorld && body->GetBulletBody())
    {
        m_dynamicsWorld->removeRigidBody(body->GetBulletBody());
    }

    // Remove from named bodies
    const std::string& name = body->GetName();
    if (!name.empty())
    {
        m_namedBodies.erase(name);
    }

    // Remove from bodies list
    auto it = std::find(m_bodies.begin(), m_bodies.end(), body);
    if (it != m_bodies.end())
    {
        m_bodies.erase(it);
    }
}

void PhysicsSystem::RemoveAllBodies()
{
    if (m_dynamicsWorld)
    {
        for (auto& body : m_bodies)
        {
            if (body && body->GetBulletBody())
            {
                m_dynamicsWorld->removeRigidBody(body->GetBulletBody());
            }
        }
    }

    m_bodies.clear();
    m_namedBodies.clear();
}

// ============================================================================
// CONSTRAINT CREATION
// ============================================================================

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateHingeConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                        std::shared_ptr<PhysicsBody> bodyB,
                                                                        const XMFLOAT3& pivotA, const XMFLOAT3& pivotB,
                                                                        const XMFLOAT3& axisA, const XMFLOAT3& axisB)
{
    if (!m_dynamicsWorld || !bodyA || !bodyB)
        return nullptr;
    if (!bodyA->GetBulletBody() || !bodyB->GetBulletBody())
        return nullptr;

    btHingeConstraint* hingeConstraint =
        new btHingeConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), ToBullet(pivotA), ToBullet(pivotB),
                              ToBullet(axisA), ToBullet(axisB));

    m_dynamicsWorld->addConstraint(hingeConstraint, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Hinge, hingeConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateSliderConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                         std::shared_ptr<PhysicsBody> bodyB,
                                                                         const XMMATRIX& frameA, const XMMATRIX& frameB)
{
    if (!m_dynamicsWorld || !bodyA || !bodyB)
        return nullptr;
    if (!bodyA->GetBulletBody() || !bodyB->GetBulletBody())
        return nullptr;

    // Convert XMMATRIX frames to btTransform
    btTransform btFrameA, btFrameB;

    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);
    XMFLOAT4 quatA;
    XMStoreFloat4(&quatA, rotA);
    btFrameA.setOrigin(btVector3(posA.x, posA.y, posA.z));
    btFrameA.setRotation(btQuaternion(quatA.x, quatA.y, quatA.z, quatA.w));

    XMVECTOR scaleB, rotB, transB;
    XMMatrixDecompose(&scaleB, &rotB, &transB, frameB);
    XMFLOAT3 posB;
    XMStoreFloat3(&posB, transB);
    XMFLOAT4 quatB;
    XMStoreFloat4(&quatB, rotB);
    btFrameB.setOrigin(btVector3(posB.x, posB.y, posB.z));
    btFrameB.setRotation(btQuaternion(quatB.x, quatB.y, quatB.z, quatB.w));

    btSliderConstraint* sliderConstraint =
        new btSliderConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), btFrameA, btFrameB, true);

    m_dynamicsWorld->addConstraint(sliderConstraint, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Slider, sliderConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateFixedConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                        std::shared_ptr<PhysicsBody> bodyB,
                                                                        const XMMATRIX& frameA, const XMMATRIX& frameB)
{
    if (!m_dynamicsWorld || !bodyA || !bodyB)
        return nullptr;
    if (!bodyA->GetBulletBody() || !bodyB->GetBulletBody())
        return nullptr;

    // Convert XMMATRIX frames to btTransform
    btTransform btFrameA, btFrameB;

    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);
    XMFLOAT4 quatA;
    XMStoreFloat4(&quatA, rotA);
    btFrameA.setOrigin(btVector3(posA.x, posA.y, posA.z));
    btFrameA.setRotation(btQuaternion(quatA.x, quatA.y, quatA.z, quatA.w));

    XMVECTOR scaleB, rotB, transB;
    XMMatrixDecompose(&scaleB, &rotB, &transB, frameB);
    XMFLOAT3 posB;
    XMStoreFloat3(&posB, transB);
    XMFLOAT4 quatB;
    XMStoreFloat4(&quatB, rotB);
    btFrameB.setOrigin(btVector3(posB.x, posB.y, posB.z));
    btFrameB.setRotation(btQuaternion(quatB.x, quatB.y, quatB.z, quatB.w));

    btFixedConstraint* fixedConstraint =
        new btFixedConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), btFrameA, btFrameB);

    m_dynamicsWorld->addConstraint(fixedConstraint, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Fixed, fixedConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreatePoint2PointConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                              std::shared_ptr<PhysicsBody> bodyB,
                                                                              const XMFLOAT3& pivotA,
                                                                              const XMFLOAT3& pivotB)
{
    if (!m_dynamicsWorld || !bodyA)
        return nullptr;
    if (!bodyA->GetBulletBody())
        return nullptr;

    btPoint2PointConstraint* p2pConstraint = nullptr;

    if (bodyB && bodyB->GetBulletBody())
    {
        // Two-body constraint
        p2pConstraint = new btPoint2PointConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), ToBullet(pivotA),
                                                    ToBullet(pivotB));
    }
    else
    {
        // Single-body constraint anchored to world
        p2pConstraint = new btPoint2PointConstraint(*bodyA->GetBulletBody(), ToBullet(pivotA));
    }

    m_dynamicsWorld->addConstraint(p2pConstraint, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Point2Point, p2pConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateConeTwistConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                            std::shared_ptr<PhysicsBody> bodyB,
                                                                            const XMMATRIX& frameA,
                                                                            const XMMATRIX& frameB, float swingSpan1,
                                                                            float swingSpan2, float twistSpan)
{
    if (!m_dynamicsWorld || !bodyA || !bodyB)
        return nullptr;
    if (!bodyA->GetBulletBody() || !bodyB->GetBulletBody())
        return nullptr;

    // Convert XMMATRIX frames to btTransform
    btTransform btFrameA, btFrameB;

    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);
    XMFLOAT4 quatA;
    XMStoreFloat4(&quatA, rotA);
    btFrameA.setOrigin(btVector3(posA.x, posA.y, posA.z));
    btFrameA.setRotation(btQuaternion(quatA.x, quatA.y, quatA.z, quatA.w));

    XMVECTOR scaleB, rotB, transB;
    XMMatrixDecompose(&scaleB, &rotB, &transB, frameB);
    XMFLOAT3 posB;
    XMStoreFloat3(&posB, transB);
    XMFLOAT4 quatB;
    XMStoreFloat4(&quatB, rotB);
    btFrameB.setOrigin(btVector3(posB.x, posB.y, posB.z));
    btFrameB.setRotation(btQuaternion(quatB.x, quatB.y, quatB.z, quatB.w));

    btConeTwistConstraint* coneTwist =
        new btConeTwistConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), btFrameA, btFrameB);

    coneTwist->setLimit(swingSpan1, swingSpan2, twistSpan);

    m_dynamicsWorld->addConstraint(coneTwist, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::ConeTwist, coneTwist);
    m_constraints.push_back(constraint);

    return constraint;
}

void PhysicsSystem::RemoveConstraint(std::shared_ptr<PhysicsConstraint> constraint)
{
    if (!constraint)
        return;

    if (m_dynamicsWorld && constraint->GetBulletConstraint())
    {
        m_dynamicsWorld->removeConstraint(constraint->GetBulletConstraint());
    }

    auto it = std::find(m_constraints.begin(), m_constraints.end(), constraint);
    if (it != m_constraints.end())
    {
        m_constraints.erase(it);
    }
}

// ============================================================================
// RAYCASTING AND OVERLAP QUERIES
// ============================================================================

RaycastHit PhysicsSystem::Raycast(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance)
{
    RaycastHit hit;
    hit.hasHit = false;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.raycastCount++;
    }

    if (!m_dynamicsWorld)
        return hit;

    btVector3 from = ToBullet(origin);
    btVector3 to = from + ToBullet(direction) * maxDistance;

    btCollisionWorld::ClosestRayResultCallback callback(from, to);
    m_dynamicsWorld->rayTest(from, to, callback);

    if (callback.hasHit())
    {
        hit.hasHit = true;
        hit.point = FromBullet(callback.m_hitPointWorld);
        hit.normal = FromBullet(callback.m_hitNormalWorld);

        btVector3 diff = callback.m_hitPointWorld - from;
        hit.distance = diff.length();

        // Find the PhysicsBody wrapper
        const btCollisionObject* hitObj = callback.m_collisionObject;
        if (hitObj)
        {
            hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
            if (hit.body)
            {
                hit.userData = hit.body->GetUserData();
            }
        }
    }

    return hit;
}

std::vector<RaycastHit> PhysicsSystem::RaycastAll(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance)
{
    std::vector<RaycastHit> hits;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.raycastCount++;
    }

    if (!m_dynamicsWorld)
        return hits;

    btVector3 from = ToBullet(origin);
    btVector3 to = from + ToBullet(direction) * maxDistance;

    btCollisionWorld::AllHitsRayResultCallback callback(from, to);
    m_dynamicsWorld->rayTest(from, to, callback);

    if (callback.hasHit())
    {
        for (int i = 0; i < callback.m_hitPointWorld.size(); i++)
        {
            RaycastHit hit;
            hit.hasHit = true;
            hit.point = FromBullet(callback.m_hitPointWorld[i]);
            hit.normal = FromBullet(callback.m_hitNormalWorld[i]);

            btVector3 diff = callback.m_hitPointWorld[i] - from;
            hit.distance = diff.length();

            const btCollisionObject* hitObj = callback.m_collisionObjects[i];
            if (hitObj)
            {
                hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
                if (hit.body)
                {
                    hit.userData = hit.body->GetUserData();
                }
            }

            hits.push_back(hit);
        }
    }

    return hits;
}

bool PhysicsSystem::SphereOverlap(const XMFLOAT3& center, float radius, std::vector<PhysicsBody*>& results)
{
    results.clear();

    if (!m_dynamicsWorld)
        return false;

    btSphereShape sphereShape(radius);
    btGhostObject ghostObject;
    ghostObject.setCollisionShape(&sphereShape);

    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(ToBullet(center));
    ghostObject.setWorldTransform(transform);

    // Use contactTest to find overlapping objects
    struct ContactCallback : public btCollisionWorld::ContactResultCallback
    {
        std::vector<PhysicsBody*>& m_results;

        ContactCallback(std::vector<PhysicsBody*>& results) : m_results(results) {}

        btScalar addSingleResult(btManifoldPoint& cp, const btCollisionObjectWrapper* colObj0Wrap, int partId0,
                                 int index0, const btCollisionObjectWrapper* colObj1Wrap, int partId1,
                                 int index1) override
        {
            const btCollisionObject* obj = colObj1Wrap->getCollisionObject();
            if (obj)
            {
                PhysicsBody* body = static_cast<PhysicsBody*>(obj->getUserPointer());
                if (body)
                {
                    // Avoid duplicates
                    if (std::find(m_results.begin(), m_results.end(), body) == m_results.end())
                    {
                        m_results.push_back(body);
                    }
                }
            }
            return btScalar(1.0);
        }
    };

    ContactCallback callback(results);
    m_dynamicsWorld->contactTest(&ghostObject, callback);

    return !results.empty();
}

bool PhysicsSystem::BoxOverlap(const XMFLOAT3& center, const XMFLOAT3& halfExtents, std::vector<PhysicsBody*>& results)
{
    results.clear();

    if (!m_dynamicsWorld)
        return false;

    btBoxShape boxShape(ToBullet(halfExtents));
    btGhostObject ghostObject;
    ghostObject.setCollisionShape(&boxShape);

    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(ToBullet(center));
    ghostObject.setWorldTransform(transform);

    struct ContactCallback : public btCollisionWorld::ContactResultCallback
    {
        std::vector<PhysicsBody*>& m_results;

        ContactCallback(std::vector<PhysicsBody*>& results) : m_results(results) {}

        btScalar addSingleResult(btManifoldPoint& cp, const btCollisionObjectWrapper* colObj0Wrap, int partId0,
                                 int index0, const btCollisionObjectWrapper* colObj1Wrap, int partId1,
                                 int index1) override
        {
            const btCollisionObject* obj = colObj1Wrap->getCollisionObject();
            if (obj)
            {
                PhysicsBody* body = static_cast<PhysicsBody*>(obj->getUserPointer());
                if (body)
                {
                    if (std::find(m_results.begin(), m_results.end(), body) == m_results.end())
                    {
                        m_results.push_back(body);
                    }
                }
            }
            return btScalar(1.0);
        }
    };

    ContactCallback callback(results);
    m_dynamicsWorld->contactTest(&ghostObject, callback);

    return !results.empty();
}

// ============================================================================
// SHAPE CASTING (SWEEP TESTS)
// ============================================================================

RaycastHit PhysicsSystem::SphereCast(float radius, const XMFLOAT3& from, const XMFLOAT3& to)
{
    RaycastHit hit;
    hit.hasHit = false;

    if (!m_dynamicsWorld)
        return hit;

    btSphereShape sphereShape(radius);

    btTransform fromTransform;
    fromTransform.setIdentity();
    fromTransform.setOrigin(ToBullet(from));

    btTransform toTransform;
    toTransform.setIdentity();
    toTransform.setOrigin(ToBullet(to));

    btCollisionWorld::ClosestConvexResultCallback callback(ToBullet(from), ToBullet(to));
    callback.m_collisionFilterGroup = 1;
    callback.m_collisionFilterMask = 0xFFFF;

    m_dynamicsWorld->convexSweepTest(&sphereShape, fromTransform, toTransform, callback);

    if (callback.hasHit())
    {
        hit.hasHit = true;
        hit.point = FromBullet(callback.m_hitPointWorld);
        hit.normal = FromBullet(callback.m_hitNormalWorld);

        btVector3 diff = callback.m_hitPointWorld - ToBullet(from);
        hit.distance = diff.length();

        const btCollisionObject* hitObj = callback.m_hitCollisionObject;
        if (hitObj)
        {
            hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
            if (hit.body)
            {
                hit.userData = hit.body->GetUserData();
            }
        }
    }

    return hit;
}

RaycastHit PhysicsSystem::BoxCast(const XMFLOAT3& halfExtents, const XMFLOAT3& from, const XMFLOAT3& to)
{
    RaycastHit hit;
    hit.hasHit = false;

    if (!m_dynamicsWorld)
        return hit;

    btBoxShape boxShape(ToBullet(halfExtents));

    btTransform fromTransform;
    fromTransform.setIdentity();
    fromTransform.setOrigin(ToBullet(from));

    btTransform toTransform;
    toTransform.setIdentity();
    toTransform.setOrigin(ToBullet(to));

    btCollisionWorld::ClosestConvexResultCallback callback(ToBullet(from), ToBullet(to));
    callback.m_collisionFilterGroup = 1;
    callback.m_collisionFilterMask = 0xFFFF;

    m_dynamicsWorld->convexSweepTest(&boxShape, fromTransform, toTransform, callback);

    if (callback.hasHit())
    {
        hit.hasHit = true;
        hit.point = FromBullet(callback.m_hitPointWorld);
        hit.normal = FromBullet(callback.m_hitNormalWorld);

        btVector3 diff = callback.m_hitPointWorld - ToBullet(from);
        hit.distance = diff.length();

        const btCollisionObject* hitObj = callback.m_hitCollisionObject;
        if (hitObj)
        {
            hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
            if (hit.body)
            {
                hit.userData = hit.body->GetUserData();
            }
        }
    }

    return hit;
}

RaycastHit PhysicsSystem::CapsuleCast(float radius, float height, const XMFLOAT3& from, const XMFLOAT3& to)
{
    RaycastHit hit;
    hit.hasHit = false;

    if (!m_dynamicsWorld)
        return hit;

    btCapsuleShape capsuleShape(radius, height);

    btTransform fromTransform;
    fromTransform.setIdentity();
    fromTransform.setOrigin(ToBullet(from));

    btTransform toTransform;
    toTransform.setIdentity();
    toTransform.setOrigin(ToBullet(to));

    btCollisionWorld::ClosestConvexResultCallback callback(ToBullet(from), ToBullet(to));
    callback.m_collisionFilterGroup = 1;
    callback.m_collisionFilterMask = 0xFFFF;

    m_dynamicsWorld->convexSweepTest(&capsuleShape, fromTransform, toTransform, callback);

    if (callback.hasHit())
    {
        hit.hasHit = true;
        hit.point = FromBullet(callback.m_hitPointWorld);
        hit.normal = FromBullet(callback.m_hitNormalWorld);

        btVector3 diff = callback.m_hitPointWorld - ToBullet(from);
        hit.distance = diff.length();

        const btCollisionObject* hitObj = callback.m_hitCollisionObject;
        if (hitObj)
        {
            hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
            if (hit.body)
            {
                hit.userData = hit.body->GetUserData();
            }
        }
    }

    return hit;
}

// ============================================================================
// DEBUG RENDERING
// ============================================================================

void PhysicsSystem::SetDebugDrawMode(int mode)
{
    if (!m_dynamicsWorld)
        return;

    m_debugDrawEnabled = (mode != 0);

    if (m_dynamicsWorld->getDebugDrawer())
    {
        m_dynamicsWorld->getDebugDrawer()->setDebugMode(mode);
    }
}

void PhysicsSystem::RenderDebug()
{
    if (!m_dynamicsWorld || !m_debugDrawEnabled)
        return;

    m_dynamicsWorld->debugDrawWorld();
}

// ============================================================================
// MATERIAL MANAGEMENT
// ============================================================================

void PhysicsSystem::RegisterMaterial(const std::string& name, const PhysicsMaterial& material)
{
    m_materials[name] = material;
}

const PhysicsMaterial* PhysicsSystem::GetMaterial(const std::string& name) const
{
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? &it->second : nullptr;
}

// ============================================================================
// METRICS
// ============================================================================

PhysicsSystem::PhysicsMetrics PhysicsSystem::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

// ============================================================================
// CONSOLE INTEGRATION METHODS
// ============================================================================

void PhysicsSystem::Console_EnableDebugDraw(bool enabled)
{
    EnableDebugDraw(enabled);
    Spark::SimpleConsole::GetInstance().LogSuccess("Physics debug draw " +
                                                   std::string(enabled ? "enabled" : "disabled"));
}

void PhysicsSystem::Console_PausePhysics(bool paused)
{
    m_paused = paused;
    Spark::SimpleConsole::GetInstance().LogSuccess("Physics simulation " + std::string(paused ? "paused" : "resumed"));
}

void PhysicsSystem::Console_SetTimeStep(float timeStep)
{
    SetTimeStep(timeStep);
    Spark::SimpleConsole::GetInstance().LogSuccess("Physics time step set to: " + std::to_string(timeStep));
}

std::string PhysicsSystem::Console_Raycast(float originX, float originY, float originZ, float dirX, float dirY,
                                           float dirZ, float maxDistance)
{
    XMFLOAT3 origin = {originX, originY, originZ};
    XMFLOAT3 direction = {dirX, dirY, dirZ};

    // Normalize direction
    XMVECTOR dirVector = XMLoadFloat3(&direction);
    dirVector = XMVector3Normalize(dirVector);
    XMStoreFloat3(&direction, dirVector);

    RaycastHit hit = Raycast(origin, direction, maxDistance);

    std::stringstream ss;
    if (hit.hasHit)
    {
        ss << "Raycast HIT:\n";
        ss << "Hit Point: (" << hit.point.x << ", " << hit.point.y << ", " << hit.point.z << ")\n";
        ss << "Hit Normal: (" << hit.normal.x << ", " << hit.normal.y << ", " << hit.normal.z << ")\n";
        ss << "Distance: " << hit.distance << "\n";
        if (hit.body)
        {
            ss << "Hit Body: " << hit.body->GetName() << "\n";
        }
    }
    else
    {
        ss << "Raycast MISS - No objects hit";
    }

    return ss.str();
}

void PhysicsSystem::Console_Reset()
{
    RemoveAllBodies();
    m_constraints.clear();
    SetGravity({0.0f, -9.8f, 0.0f});
    m_paused = false;

    Spark::SimpleConsole::GetInstance().LogSuccess("Physics system reset complete");
}

PhysicsSystem::PhysicsMetrics PhysicsSystem::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

std::string PhysicsSystem::Console_ListBodies() const
{
    std::stringstream ss;
    ss << "=== Physics Bodies (" << m_bodies.size() << ") ===\n";

    for (const auto& body : m_bodies)
    {
        if (body)
        {
            ss << body->GetName() << " - " << PhysicsBodyTypeToString(body->GetType());
            auto pos = body->GetPosition();
            ss << " at (" << pos.x << ", " << pos.y << ", " << pos.z << ")\n";
        }
    }

    return ss.str();
}

std::string PhysicsSystem::Console_GetBodyInfo(const std::string& name) const
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        return it->second->GetInfo();
    }

    return "Physics body not found: " + name;
}

bool PhysicsSystem::Console_CreateBody(const std::string& name, const std::string& type, float x, float y, float z)
{
    PhysicsBodyDesc desc;
    desc.name = name;
    desc.position = {x, y, z};
    desc.type = StringToPhysicsBodyType(type);
    desc.shape.type = CollisionShapeType::Box; // Default to box
    desc.mass = (desc.type == PhysicsBodyType::Static) ? 0.0f : 1.0f;

    auto body = CreateBody(desc);
    return body != nullptr;
}

bool PhysicsSystem::Console_RemoveBody(const std::string& name)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end())
    {
        RemoveBody(it->second);
        return true;
    }

    return false;
}

void PhysicsSystem::Console_SetGravity(float x, float y, float z)
{
    SetGravity({x, y, z});
    Spark::SimpleConsole::GetInstance().LogSuccess("Gravity set to (" + std::to_string(x) + ", " + std::to_string(y) +
                                                   ", " + std::to_string(z) + ")");
}

void PhysicsSystem::Console_SetBodyProperty(const std::string& name, const std::string& property, float value)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        it->second->Console_SetProperty(property, value);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set " + property + " = " + std::to_string(value) + " for " +
                                                       name);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Physics body not found: " + name);
    }
}

void PhysicsSystem::Console_ApplyForce(const std::string& name, float x, float y, float z)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        it->second->ApplyForce({x, y, z});
        Spark::SimpleConsole::GetInstance().LogSuccess("Applied force (" + std::to_string(x) + ", " +
                                                       std::to_string(y) + ", " + std::to_string(z) + ") to " + name);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Physics body not found: " + name);
    }
}

void PhysicsSystem::Console_ApplyImpulse(const std::string& name, float x, float y, float z)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        it->second->ApplyImpulse({x, y, z});
        Spark::SimpleConsole::GetInstance().LogSuccess("Applied impulse (" + std::to_string(x) + ", " +
                                                       std::to_string(y) + ", " + std::to_string(z) + ") to " + name);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Physics body not found: " + name);
    }
}

// ============================================================================
// UTILITY FUNCTIONS IMPLEMENTATION - MISSING GLOBAL FUNCTIONS
// ============================================================================

std::string PhysicsBodyTypeToString(PhysicsBodyType type)
{
    switch (type)
    {
    case PhysicsBodyType::Static:
        return "Static";
    case PhysicsBodyType::Kinematic:
        return "Kinematic";
    case PhysicsBodyType::Dynamic:
        return "Dynamic";
    default:
        return "Unknown";
    }
}

PhysicsBodyType StringToPhysicsBodyType(const std::string& str)
{
    if (str == "Static" || str == "static")
        return PhysicsBodyType::Static;
    if (str == "Kinematic" || str == "kinematic")
        return PhysicsBodyType::Kinematic;
    if (str == "Dynamic" || str == "dynamic")
        return PhysicsBodyType::Dynamic;
    return PhysicsBodyType::Dynamic; // Default
}

std::string CollisionShapeTypeToString(CollisionShapeType type)
{
    switch (type)
    {
    case CollisionShapeType::Box:
        return "Box";
    case CollisionShapeType::Sphere:
        return "Sphere";
    case CollisionShapeType::Capsule:
        return "Capsule";
    case CollisionShapeType::Cylinder:
        return "Cylinder";
    case CollisionShapeType::Cone:
        return "Cone";
    case CollisionShapeType::Mesh:
        return "Mesh";
    case CollisionShapeType::ConvexHull:
        return "ConvexHull";
    case CollisionShapeType::Heightfield:
        return "Heightfield";
    case CollisionShapeType::Compound:
        return "Compound";
    default:
        return "Unknown";
    }
}

CollisionShapeType StringToCollisionShapeType(const std::string& str)
{
    if (str == "Box" || str == "box")
        return CollisionShapeType::Box;
    if (str == "Sphere" || str == "sphere")
        return CollisionShapeType::Sphere;
    if (str == "Capsule" || str == "capsule")
        return CollisionShapeType::Capsule;
    if (str == "Cylinder" || str == "cylinder")
        return CollisionShapeType::Cylinder;
    if (str == "Cone" || str == "cone")
        return CollisionShapeType::Cone;
    if (str == "Mesh" || str == "mesh")
        return CollisionShapeType::Mesh;
    if (str == "ConvexHull" || str == "convexhull")
        return CollisionShapeType::ConvexHull;
    if (str == "Heightfield" || str == "heightfield")
        return CollisionShapeType::Heightfield;
    if (str == "Compound" || str == "compound")
        return CollisionShapeType::Compound;
    return CollisionShapeType::Box; // Default
}

std::string ConstraintTypeToString(ConstraintType type)
{
    switch (type)
    {
    case ConstraintType::Point2Point:
        return "Point2Point";
    case ConstraintType::Hinge:
        return "Hinge";
    case ConstraintType::Slider:
        return "Slider";
    case ConstraintType::ConeTwist:
        return "ConeTwist";
    case ConstraintType::Generic6DOF:
        return "Generic6DOF";
    case ConstraintType::Fixed:
        return "Fixed";
    default:
        return "Unknown";
    }
}

ConstraintType StringToConstraintType(const std::string& str)
{
    if (str == "Point2Point" || str == "point2point")
        return ConstraintType::Point2Point;
    if (str == "Hinge" || str == "hinge")
        return ConstraintType::Hinge;
    if (str == "Slider" || str == "slider")
        return ConstraintType::Slider;
    if (str == "ConeTwist" || str == "conetwist")
        return ConstraintType::ConeTwist;
    if (str == "Generic6DOF" || str == "generic6dof")
        return ConstraintType::Generic6DOF;
    if (str == "Fixed" || str == "fixed")
        return ConstraintType::Fixed;
    return ConstraintType::Fixed; // Default
}
