/**
 * @file PhysicsSystemStub.cpp
 * @brief No-op implementation of PhysicsSystem when Jolt Physics is unavailable.
 *
 * Compiled only when SPARK_JOLT_PHYSICS_AVAILABLE is NOT set.  Every public
 * method returns a safe default so the rest of the engine and game link without
 * the Jolt SDK.
 */

#include "Core/Platform.h"
#include "PhysicsSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/Hash.h"
#include "Utils/Validate.h"
#include <sstream>

// Global physics system pointer (stub — always null)
::PhysicsSystem* g_physicsSystem = nullptr;

// ============================================================================
// Utility functions
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
    }
    return "Unknown";
}

PhysicsBodyType StringToPhysicsBodyType(const std::string& str)
{
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "Static"_hash64:
        return PhysicsBodyType::Static;
    case "Kinematic"_hash64:
        return PhysicsBodyType::Kinematic;
    default:
        return PhysicsBodyType::Dynamic;
    }
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
    }
    return "Unknown";
}

CollisionShapeType StringToCollisionShapeType(const std::string& str)
{
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "Sphere"_hash64:
        return CollisionShapeType::Sphere;
    case "Capsule"_hash64:
        return CollisionShapeType::Capsule;
    case "Cylinder"_hash64:
        return CollisionShapeType::Cylinder;
    case "Cone"_hash64:
        return CollisionShapeType::Cone;
    case "Mesh"_hash64:
        return CollisionShapeType::Mesh;
    case "ConvexHull"_hash64:
        return CollisionShapeType::ConvexHull;
    case "Heightfield"_hash64:
        return CollisionShapeType::Heightfield;
    case "Compound"_hash64:
        return CollisionShapeType::Compound;
    default:
        return CollisionShapeType::Box;
    }
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
    }
    return "Unknown";
}

ConstraintType StringToConstraintType(const std::string& str)
{
    using namespace Spark::HashLiterals;
    switch (Spark::FNV1a64(str))
    {
    case "Hinge"_hash64:
        return ConstraintType::Hinge;
    case "Slider"_hash64:
        return ConstraintType::Slider;
    case "ConeTwist"_hash64:
        return ConstraintType::ConeTwist;
    case "Generic6DOF"_hash64:
        return ConstraintType::Generic6DOF;
    case "Fixed"_hash64:
        return ConstraintType::Fixed;
    default:
        return ConstraintType::Point2Point;
    }
}

// ============================================================================
// PhysicsBody — stub
// ============================================================================

PhysicsBody::PhysicsBody(const PhysicsBodyDesc& desc, uint32_t /*joltBodyID*/) : m_desc(desc), m_joltBodyID(0) {}

PhysicsBody::~PhysicsBody() = default;

XMFLOAT3 PhysicsBody::GetPosition() const
{
    return m_desc.position;
}

void PhysicsBody::SetPosition(const XMFLOAT3& position)
{
    m_desc.position = position;
}

XMFLOAT3 PhysicsBody::GetRotation() const
{
    return m_desc.rotation;
}

void PhysicsBody::SetRotation(const XMFLOAT3& rotation)
{
    m_desc.rotation = rotation;
}

XMMATRIX PhysicsBody::GetTransform() const
{
    return XMMatrixTranslation(m_desc.position.x, m_desc.position.y, m_desc.position.z);
}

void PhysicsBody::SetTransform(const XMMATRIX& /*transform*/) {}

XMFLOAT3 PhysicsBody::GetLinearVelocity() const
{
    return m_desc.linearVelocity;
}

void PhysicsBody::SetLinearVelocity(const XMFLOAT3& velocity)
{
    m_desc.linearVelocity = velocity;
}

XMFLOAT3 PhysicsBody::GetAngularVelocity() const
{
    return m_desc.angularVelocity;
}

void PhysicsBody::SetAngularVelocity(const XMFLOAT3& velocity)
{
    m_desc.angularVelocity = velocity;
}

void PhysicsBody::ApplyForce(const XMFLOAT3& /*force*/, const XMFLOAT3& /*relativePos*/) {}

void PhysicsBody::ApplyImpulse(const XMFLOAT3& /*impulse*/, const XMFLOAT3& /*relativePos*/) {}

void PhysicsBody::ApplyTorque(const XMFLOAT3& /*torque*/) {}

void PhysicsBody::ApplyTorqueImpulse(const XMFLOAT3& /*torque*/) {}

float PhysicsBody::GetMass() const
{
    return m_desc.mass;
}

void PhysicsBody::SetMass(float mass)
{
    m_desc.mass = mass;
}

void PhysicsBody::SetMaterial(const PhysicsMaterial& material)
{
    m_desc.material = material;
}

void PhysicsBody::SetActive(bool /*active*/) {}

bool PhysicsBody::IsActive() const
{
    return false;
}

void PhysicsBody::SetKinematic(bool kinematic)
{
    m_desc.isKinematic = kinematic;
    m_desc.type = kinematic ? PhysicsBodyType::Kinematic : PhysicsBodyType::Dynamic;
}

bool PhysicsBody::IsKinematic() const
{
    return m_desc.type == PhysicsBodyType::Kinematic;
}

void PhysicsBody::SetTrigger(bool trigger)
{
    m_desc.isTrigger = trigger;
}

bool PhysicsBody::IsTrigger() const
{
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
    // Stub: no Jolt broadphase to update
}

std::string PhysicsBody::GetInfo() const
{
    std::stringstream ss;
    ss << "Body: " << m_desc.name << "\n";
    ss << "  Type: " << PhysicsBodyTypeToString(m_desc.type) << "\n";
    ss << "  Position: (" << m_desc.position.x << ", " << m_desc.position.y << ", " << m_desc.position.z << ")\n";
    ss << "  Mass: " << m_desc.mass << " kg\n";
    ss << "  [Jolt Physics not available - stub implementation]\n";
    return ss.str();
}

void PhysicsBody::Console_SetProperty(const std::string& property, float value)
{
    if (property == "mass")
        m_desc.mass = value;
    else if (property == "friction")
        m_desc.material.friction = value;
    else if (property == "restitution")
        m_desc.material.restitution = value;
    else if (property == "linearDamping")
        m_desc.material.linearDamping = value;
    else if (property == "angularDamping")
        m_desc.material.angularDamping = value;
}

void PhysicsBody::Console_ApplyForce(float /*x*/, float /*y*/, float /*z*/) {}

XMFLOAT3 PhysicsBody::GetInterpolatedPosition(float /*alpha*/) const
{
    return m_desc.position;
}

XMMATRIX PhysicsBody::GetInterpolatedTransform(float /*alpha*/) const
{
    return GetTransform();
}

void PhysicsBody::StoreCurrentState() {}

void PhysicsBody::UpdateCurrentState() {}

// ============================================================================
// PhysicsConstraint — stub
// ============================================================================

PhysicsConstraint::PhysicsConstraint(ConstraintType type, JPH::Constraint* /*joltConstraint*/)
    : m_type(type), m_joltConstraint(nullptr)
{
}

PhysicsConstraint::~PhysicsConstraint() = default;

void PhysicsConstraint::SetEnabled(bool /*enabled*/) {}

bool PhysicsConstraint::IsEnabled() const
{
    return false;
}

void PhysicsConstraint::SetBreakingThreshold(float /*threshold*/) {}

float PhysicsConstraint::GetBreakingThreshold() const
{
    return 0.0f;
}

// ============================================================================
// PhysicsSystem — stub
// ============================================================================

PhysicsSystem::PhysicsSystem()
{
    m_metrics = {};
}

PhysicsSystem::~PhysicsSystem()
{
    Shutdown();
}

HRESULT PhysicsSystem::Initialize()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_LOG_WARN(Spark::LogCategory::Physics, "Jolt Physics not available — physics simulation disabled");
    return S_OK;
}

void PhysicsSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_LOG_INFO(Spark::LogCategory::Physics, "PhysicsSystem stub shutting down");
    m_bodies.clear();
    m_constraints.clear();
    m_namedBodies.clear();
    m_bodyIDMap.clear();
    m_shapeCache.clear();
    m_materials.clear();
}

void PhysicsSystem::Update(float /*deltaTime*/) {}

void PhysicsSystem::SetGravity(const XMFLOAT3& /*gravity*/) {}

XMFLOAT3 PhysicsSystem::GetGravity() const
{
    return XMFLOAT3(0.0f, -9.81f, 0.0f);
}

std::shared_ptr<PhysicsBody> PhysicsSystem::CreateBody(const PhysicsBodyDesc& desc)
{
    auto body = std::make_shared<PhysicsBody>(desc, 0);
    m_bodies.push_back(body);
    if (!desc.name.empty())
    {
        m_namedBodies[desc.name] = body;
    }
    return body;
}

void PhysicsSystem::RemoveBody(std::shared_ptr<PhysicsBody> body)
{
    SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Physics, body);

    if (!body->GetName().empty())
    {
        m_namedBodies.erase(body->GetName());
    }
    m_bodies.erase(std::remove(m_bodies.begin(), m_bodies.end(), body), m_bodies.end());
}

void PhysicsSystem::RemoveAllBodies()
{
    m_bodies.clear();
    m_namedBodies.clear();
    m_bodyIDMap.clear();
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateHingeConstraint(
    std::shared_ptr<PhysicsBody> /*bodyA*/, std::shared_ptr<PhysicsBody> /*bodyB*/, const XMFLOAT3& /*pivotA*/,
    const XMFLOAT3& /*pivotB*/, const XMFLOAT3& /*axisA*/, const XMFLOAT3& /*axisB*/)
{
    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Hinge, nullptr);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateSliderConstraint(std::shared_ptr<PhysicsBody> /*bodyA*/,
                                                                         std::shared_ptr<PhysicsBody> /*bodyB*/,
                                                                         const XMMATRIX& /*frameA*/,
                                                                         const XMMATRIX& /*frameB*/)
{
    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Slider, nullptr);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreatePoint2PointConstraint(std::shared_ptr<PhysicsBody> /*bodyA*/,
                                                                              std::shared_ptr<PhysicsBody> /*bodyB*/,
                                                                              const XMFLOAT3& /*pivotA*/,
                                                                              const XMFLOAT3& /*pivotB*/)
{
    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Point2Point, nullptr);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateConeTwistConstraint(
    std::shared_ptr<PhysicsBody> /*bodyA*/, std::shared_ptr<PhysicsBody> /*bodyB*/, const XMMATRIX& /*frameA*/,
    const XMMATRIX& /*frameB*/, float /*swingSpan1*/, float /*swingSpan2*/, float /*twistSpan*/)
{
    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::ConeTwist, nullptr);
    m_constraints.push_back(constraint);
    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateFixedConstraint(std::shared_ptr<PhysicsBody> /*bodyA*/,
                                                                        std::shared_ptr<PhysicsBody> /*bodyB*/,
                                                                        const XMMATRIX& /*frameA*/,
                                                                        const XMMATRIX& /*frameB*/)
{
    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Fixed, nullptr);
    m_constraints.push_back(constraint);
    return constraint;
}

void PhysicsSystem::RemoveConstraint(std::shared_ptr<PhysicsConstraint> constraint)
{
    m_constraints.erase(std::remove(m_constraints.begin(), m_constraints.end(), constraint), m_constraints.end());
}

RaycastHit PhysicsSystem::Raycast(const XMFLOAT3& /*origin*/, const XMFLOAT3& /*direction*/, float /*maxDistance*/)
{
    return RaycastHit{};
}

std::vector<RaycastHit> PhysicsSystem::RaycastAll(const XMFLOAT3& /*origin*/, const XMFLOAT3& /*direction*/,
                                                  float /*maxDistance*/)
{
    return {};
}

bool PhysicsSystem::SphereOverlap(const XMFLOAT3& /*center*/, float /*radius*/, std::vector<PhysicsBody*>& results)
{
    results.clear();
    return false;
}

bool PhysicsSystem::BoxOverlap(const XMFLOAT3& /*center*/, const XMFLOAT3& /*halfExtents*/,
                               std::vector<PhysicsBody*>& results)
{
    results.clear();
    return false;
}

RaycastHit PhysicsSystem::SphereCast(float /*radius*/, const XMFLOAT3& /*from*/, const XMFLOAT3& /*to*/)
{
    return RaycastHit{};
}

RaycastHit PhysicsSystem::BoxCast(const XMFLOAT3& /*halfExtents*/, const XMFLOAT3& /*from*/, const XMFLOAT3& /*to*/)
{
    return RaycastHit{};
}

RaycastHit PhysicsSystem::CapsuleCast(float /*radius*/, float /*height*/, const XMFLOAT3& /*from*/,
                                      const XMFLOAT3& /*to*/)
{
    return RaycastHit{};
}

void PhysicsSystem::SetDebugDrawMode(int /*mode*/) {}

void PhysicsSystem::RenderDebug() {}

void PhysicsSystem::RegisterMaterial(const std::string& name, const PhysicsMaterial& material)
{
    m_materials[name] = material;
}

const PhysicsMaterial* PhysicsSystem::GetMaterial(const std::string& name) const
{
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? &it->second : nullptr;
}

PhysicsSystem::PhysicsMetrics PhysicsSystem::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

PhysicsSystem::PhysicsMetrics PhysicsSystem::Console_GetMetrics() const
{
    return GetMetrics();
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
    return "Body '" + name + "' not found";
}

bool PhysicsSystem::Console_CreateBody(const std::string& name, const std::string& type, float x, float y, float z)
{
    PhysicsBodyDesc desc;
    desc.name = name;
    desc.type = StringToPhysicsBodyType(type);
    desc.position = XMFLOAT3(x, y, z);
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
    SetGravity(XMFLOAT3(x, y, z));
}

void PhysicsSystem::Console_SetBodyProperty(const std::string& name, const std::string& property, float value)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        it->second->Console_SetProperty(property, value);
    }
}

void PhysicsSystem::Console_ApplyForce(const std::string& name, float x, float y, float z)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        it->second->Console_ApplyForce(x, y, z);
    }
}

void PhysicsSystem::Console_ApplyImpulse(const std::string& name, float x, float y, float z)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        it->second->ApplyImpulse(XMFLOAT3(x, y, z));
    }
}

void PhysicsSystem::Console_EnableDebugDraw(bool enabled)
{
    m_debugDrawEnabled = enabled;
}

void PhysicsSystem::Console_PausePhysics(bool paused)
{
    m_paused = paused;
}

void PhysicsSystem::Console_SetTimeStep(float timeStep)
{
    if (timeStep > 0.0f)
    {
        m_timeStep = timeStep;
    }
}

std::string PhysicsSystem::Console_Raycast(float originX, float originY, float originZ, float dirX, float dirY,
                                           float dirZ, float maxDistance)
{
    auto hit = Raycast(XMFLOAT3(originX, originY, originZ), XMFLOAT3(dirX, dirY, dirZ), maxDistance);
    if (hit.hasHit)
    {
        std::stringstream ss;
        ss << "Hit: (" << hit.point.x << ", " << hit.point.y << ", " << hit.point.z << ") dist=" << hit.distance;
        if (hit.body)
            ss << " body=" << hit.body->GetName();
        return ss.str();
    }
    return "No hit";
}

void PhysicsSystem::Console_Reset()
{
    RemoveAllBodies();
    m_constraints.clear();
    m_materials.clear();
    m_paused = false;
    m_debugDrawEnabled = false;
    m_timeStep = 1.0f / 60.0f;
    m_maxSubsteps = 10;
}

void PhysicsSystem::UpdateMetrics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.totalRigidBodies = static_cast<uint32_t>(m_bodies.size());
    m_metrics.activeRigidBodies = 0;
    m_metrics.activeConstraints = static_cast<uint32_t>(m_constraints.size());
    m_metrics.collisionPairs = 0;
    m_metrics.broadphaseProxies = 0;
    m_metrics.debugDrawEnabled = m_debugDrawEnabled;
    m_metrics.timeStep = m_timeStep;
}

void PhysicsSystem::ProcessCollisions() {}

PhysicsBody* PhysicsSystem::FindBodyByJoltID(uint32_t /*joltBodyID*/) const
{
    return nullptr;
}

size_t PhysicsSystem::HashShape(const CollisionShapeDesc& desc) const
{
    size_t hash = std::hash<int>{}(static_cast<int>(desc.type));
    Spark::CombineHash(hash, std::hash<float>{}(desc.dimensions.x));
    Spark::CombineHash(hash, std::hash<float>{}(desc.dimensions.y));
    Spark::CombineHash(hash, std::hash<float>{}(desc.dimensions.z));
    Spark::CombineHash(hash, std::hash<float>{}(desc.radius));
    Spark::CombineHash(hash, std::hash<float>{}(desc.height));
    return hash;
}
