#include "../Core/Platform.h"
/**
 * @file PhysicsSystemQueries.cpp
 * @brief Body lifecycle, debug rendering, materials, metrics, buoyancy, state
 *        serialization, group filters, compound shapes, and center-of-mass for PhysicsSystem
 * @author Spark Engine Team
 * @date 2025
 *
 * Constraint creation lives in PhysicsConstraints.cpp.
 * Constraint motor control lives in PhysicsMotorControl.cpp.
 * Spatial queries (raycast, overlap, sweep) live in PhysicsSpatialQueries.cpp.
 */

#include "PhysicsSystem.h"
#include "Utils/Assert.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/Validate.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

#include <algorithm>
#include <mutex>

using namespace DirectX;

// Legacy global removed — PhysicsBodyImpl now uses EngineContext::Get()->GetPhysics()

// ============================================================================
// BODY CREATION / REMOVAL
// ============================================================================

std::shared_ptr<PhysicsBody> PhysicsSystem::CreateBody(const PhysicsBodyDesc& desc)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Physics, m_joltSystem, nullptr);

    // PhysicsSystem is registered with EngineContext at engine startup

    // Create collision shape
    void* shapePtr = CreateCollisionShape(desc.shape);
    if (!shapePtr)
        return nullptr;

    auto* shapeRef = static_cast<JPH::ShapeRefC*>(shapePtr);

    // Determine motion type
    JPH::EMotionType motionType = JPH::EMotionType::Dynamic;
    if (desc.type == PhysicsBodyType::Static || (desc.mass <= 0.0f && !desc.isKinematic))
    {
        motionType = JPH::EMotionType::Static;
    }
    else if (desc.isKinematic || desc.type == PhysicsBodyType::Kinematic)
    {
        motionType = JPH::EMotionType::Kinematic;
    }

    // Determine object layer
    JPH::ObjectLayer objectLayer = 1; // MOVING
    if (motionType == JPH::EMotionType::Static)
        objectLayer = 0; // NON_MOVING
    if (desc.isTrigger)
        objectLayer = 2; // TRIGGER

    // Create body settings
    JPH::BodyCreationSettings bodySettings(
        *shapeRef, JPH::RVec3(desc.position.x, desc.position.y, desc.position.z),
        JPH::Quat::sEulerAngles(JPH::Vec3(desc.rotation.x, desc.rotation.y, desc.rotation.z)), motionType, objectLayer);

    // Set material properties
    bodySettings.mFriction = desc.material.friction;
    bodySettings.mRestitution = desc.material.restitution;
    bodySettings.mLinearDamping = desc.material.linearDamping;
    bodySettings.mAngularDamping = desc.material.angularDamping;

    // Set mass for dynamic bodies
    if (motionType == JPH::EMotionType::Dynamic && desc.mass > 0.0f)
    {
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = desc.mass;
    }

    // Set initial velocities
    if (motionType == JPH::EMotionType::Dynamic)
    {
        bodySettings.mLinearVelocity = JPH::Vec3(desc.linearVelocity.x, desc.linearVelocity.y, desc.linearVelocity.z);
        bodySettings.mAngularVelocity =
            JPH::Vec3(desc.angularVelocity.x, desc.angularVelocity.y, desc.angularVelocity.z);
    }

    // Set trigger (sensor) flag
    if (desc.isTrigger)
    {
        bodySettings.mIsSensor = true;
    }

    // Set CCD / motion quality
    if (desc.motionQuality == MotionQuality::LinearCast)
    {
        bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    }

    // Set allowed degrees of freedom
    bodySettings.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(static_cast<uint8_t>(desc.allowedDOFs));

    // Set gravity factor
    bodySettings.mGravityFactor = desc.gravityFactor;

    // Set velocity limits
    bodySettings.mMaxLinearVelocity = desc.maxLinearVelocity;
    bodySettings.mMaxAngularVelocity = desc.maxAngularVelocity;

    // Set collision group filtering (GroupFilterTable)
    if (desc.collisionGroupDesc.groupFilterID != 0 &&
        desc.collisionGroupDesc.groupFilterID <= m_groupFilterTables.size())
    {
        auto* tableRef = static_cast<JPH::Ref<JPH::GroupFilterTable>*>(
            m_groupFilterTables[desc.collisionGroupDesc.groupFilterID - 1]);
        bodySettings.mCollisionGroup.SetGroupFilter(tableRef->GetPtr());
        bodySettings.mCollisionGroup.SetGroupID(desc.collisionGroupDesc.groupID);
        bodySettings.mCollisionGroup.SetSubGroupID(desc.collisionGroupDesc.subGroupID);
    }

    // Create the body via Jolt
    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(bodySettings, JPH::EActivation::Activate);

    if (bodyID.IsInvalid())
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Physics, "Failed to create Jolt body: %s", desc.name.c_str());
        return nullptr;
    }

    // Create the PhysicsBody wrapper
    auto body = std::make_shared<PhysicsBody>(desc, bodyID.GetIndexAndSequenceNumber());
    body->SetCollisionGroup(desc.collisionGroup);
    body->SetCollisionMask(desc.collisionMask);

    // Store user data on the Jolt body for collision lookup
    bodyInterface.SetUserData(bodyID, reinterpret_cast<uint64_t>(body.get()));

    m_bodies.push_back(body);
    m_bodyIDMap[bodyID.GetIndexAndSequenceNumber()] = body.get();

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

    // Remove from Jolt world
    if (m_joltSystem)
    {
        auto& bodyInterface = m_joltSystem->GetBodyInterface();
        JPH::BodyID bodyID(body->GetJoltBodyID());
        if (bodyInterface.IsAdded(bodyID))
        {
            bodyInterface.RemoveBody(bodyID);
            bodyInterface.DestroyBody(bodyID);
        }
    }

    // Remove from ID map
    m_bodyIDMap.erase(body->GetJoltBodyID());

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
    if (m_joltSystem)
    {
        auto& bodyInterface = m_joltSystem->GetBodyInterface();
        for (auto& body : m_bodies)
        {
            if (body)
            {
                JPH::BodyID bodyID(body->GetJoltBodyID());
                if (bodyInterface.IsAdded(bodyID))
                {
                    bodyInterface.RemoveBody(bodyID);
                    bodyInterface.DestroyBody(bodyID);
                }
            }
        }
    }

    m_bodies.clear();
    m_namedBodies.clear();
    m_bodyIDMap.clear();
}

// ============================================================================
// CHARACTER CONTROLLER
// ============================================================================

std::unique_ptr<CharacterController> PhysicsSystem::CreateCharacterController(const CharacterControllerDesc& desc)
{
    return std::make_unique<CharacterController>(this, desc);
}

std::unique_ptr<VehiclePhysics> PhysicsSystem::CreateVehicle(std::shared_ptr<PhysicsBody> body, const VehicleDesc& desc)
{
    return std::make_unique<VehiclePhysics>(this, body, desc);
}

std::unique_ptr<Ragdoll> PhysicsSystem::CreateRagdoll(const RagdollDesc& desc)
{
    return std::make_unique<Ragdoll>(this, desc);
}

std::unique_ptr<SoftBody> PhysicsSystem::CreateSoftBody(const SoftBodyDesc& desc)
{
    return std::make_unique<SoftBody>(this, desc);
}

void PhysicsSystem::SetSurfaceVelocity(std::shared_ptr<PhysicsBody> body, const XMFLOAT3& velocity)
{
    if (!body)
        return;

    uint32_t bodyID = body->GetJoltBodyID();

    // Store surface velocity for lookup by the contact listener.
    // The SparkContactListener reads this map during OnContactAdded/OnContactPersisted
    // and applies it to ContactSettings::mRelativeLinearSurfaceVelocity.
    std::lock_guard<std::mutex> lock(m_surfaceVelocityMutex);

    if (velocity.x == 0.0f && velocity.y == 0.0f && velocity.z == 0.0f)
    {
        m_surfaceVelocities.erase(bodyID);
    }
    else
    {
        m_surfaceVelocities[bodyID] = velocity;
    }
}

// ============================================================================
// DEBUG RENDERING
// ============================================================================

void PhysicsSystem::SetDebugDrawMode(int mode)
{
    m_debugDrawEnabled = (mode != 0);
}

void PhysicsSystem::RenderDebug()
{
    if (!m_joltSystem || !m_debugDrawEnabled)
        return;

    // Iterate all bodies and collect debug visualization data.
    // The PhysicsDebugRenderer data collector (separate class) can be used by
    // the graphics engine to render wireframes, AABBs, velocity vectors, etc.
    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    for (const auto& body : m_bodies)
    {
        if (!body)
            continue;
        JPH::BodyID bodyID(body->GetJoltBodyID());
        if (!bodyInterface.IsAdded(bodyID))
            continue;

        // Get body world-space bounds via TransformedShape
        JPH::TransformedShape ts = bodyInterface.GetTransformedShape(bodyID);
        JPH::AABox aabb = ts.GetWorldSpaceBounds();
        bool isActive = bodyInterface.IsActive(bodyID);

        // Update metrics with debug info
        if (isActive)
        {
            std::lock_guard<std::mutex> lock(m_metricsMutex);
            m_metrics.activeRigidBodies++;
        }

        (void)aabb; // Consumed by the debug renderer when wired to graphics
    }
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
// BUOYANCY
// ============================================================================

void PhysicsSystem::ApplyBuoyancy(std::shared_ptr<PhysicsBody> body, const XMFLOAT3& waterSurface,
                                  const XMFLOAT3& waterNormal, float waterDensity, float linearDrag, float angularDrag)
{
    if (!m_joltSystem || !body)
        return;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());
    if (!bodyInterface.IsAdded(bodyID))
        return;

    JPH::Body* joltBody = const_cast<JPH::Body*>(m_joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!joltBody || !joltBody->IsDynamic())
        return;

    // Use Jolt's built-in buoyancy application
    JPH::Vec3 gravity = m_joltSystem->GetGravity();
    JPH::RVec3 surfacePos(waterSurface.x, waterSurface.y, waterSurface.z);
    JPH::Vec3 surfaceNorm(waterNormal.x, waterNormal.y, waterNormal.z);

    // ApplyBuoyancyImpulse(surfacePosition, surfaceNormal, buoyancy, linearDrag, angularDrag, fluidVelocity, gravity,
    // dt)
    joltBody->ApplyBuoyancyImpulse(surfacePos, surfaceNorm, waterDensity, linearDrag, angularDrag, JPH::Vec3::sZero(),
                                   gravity, m_timeStep);
}

// ============================================================================
// STATE SERIALIZATION
// ============================================================================

bool PhysicsSystem::SaveState(std::vector<uint8_t>& outBuffer) const
{
    if (!m_joltSystem)
        return false;

    // Serialize body positions, rotations, velocities, and activation state
    outBuffer.clear();

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    uint32_t bodyCount = static_cast<uint32_t>(m_bodies.size());

    // Reserve header + per-body data
    // Header: [bodyCount:4]
    // Per body: [joltID:4][posX:4][posY:4][posZ:4][rotX:4][rotY:4][rotZ:4][rotW:4]
    //           [linVelX:4][linVelY:4][linVelZ:4][angVelX:4][angVelY:4][angVelZ:4][active:1]
    size_t perBodySize = 4 + 3 * 4 + 4 * 4 + 3 * 4 + 3 * 4 + 1; // 57 bytes
    outBuffer.resize(4 + bodyCount * perBodySize);

    uint8_t* ptr = outBuffer.data();
    std::memcpy(ptr, &bodyCount, 4);
    ptr += 4;

    for (const auto& body : m_bodies)
    {
        if (!body)
            continue;

        uint32_t id = body->GetJoltBodyID();
        std::memcpy(ptr, &id, 4);
        ptr += 4;

        JPH::BodyID bodyID(id);
        if (bodyInterface.IsAdded(bodyID))
        {
            JPH::RVec3 pos = bodyInterface.GetPosition(bodyID);
            float px = static_cast<float>(pos.GetX()), py = static_cast<float>(pos.GetY()),
                  pz = static_cast<float>(pos.GetZ());
            std::memcpy(ptr, &px, 4);
            ptr += 4;
            std::memcpy(ptr, &py, 4);
            ptr += 4;
            std::memcpy(ptr, &pz, 4);
            ptr += 4;

            JPH::Quat rot = bodyInterface.GetRotation(bodyID);
            float rx = rot.GetX(), ry = rot.GetY(), rz = rot.GetZ(), rw = rot.GetW();
            std::memcpy(ptr, &rx, 4);
            ptr += 4;
            std::memcpy(ptr, &ry, 4);
            ptr += 4;
            std::memcpy(ptr, &rz, 4);
            ptr += 4;
            std::memcpy(ptr, &rw, 4);
            ptr += 4;

            JPH::Vec3 linVel = bodyInterface.GetLinearVelocity(bodyID);
            float lvx = linVel.GetX(), lvy = linVel.GetY(), lvz = linVel.GetZ();
            std::memcpy(ptr, &lvx, 4);
            ptr += 4;
            std::memcpy(ptr, &lvy, 4);
            ptr += 4;
            std::memcpy(ptr, &lvz, 4);
            ptr += 4;

            JPH::Vec3 angVel = bodyInterface.GetAngularVelocity(bodyID);
            float avx = angVel.GetX(), avy = angVel.GetY(), avz = angVel.GetZ();
            std::memcpy(ptr, &avx, 4);
            ptr += 4;
            std::memcpy(ptr, &avy, 4);
            ptr += 4;
            std::memcpy(ptr, &avz, 4);
            ptr += 4;

            uint8_t active = bodyInterface.IsActive(bodyID) ? 1 : 0;
            *ptr++ = active;
        }
        else
        {
            std::memset(ptr, 0, perBodySize - 4);
            ptr += perBodySize - 4;
        }
    }

    return true;
}

bool PhysicsSystem::LoadState(const std::vector<uint8_t>& buffer)
{
    if (!m_joltSystem || buffer.size() < 4)
        return false;

    const uint8_t* ptr = buffer.data();
    uint32_t bodyCount = 0;
    std::memcpy(&bodyCount, ptr, 4);
    ptr += 4;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    size_t perBodySize = 4 + 3 * 4 + 4 * 4 + 3 * 4 + 3 * 4 + 1;

    for (uint32_t i = 0; i < bodyCount; i++)
    {
        if (static_cast<size_t>(ptr - buffer.data()) + perBodySize > buffer.size())
            break;

        uint32_t id;
        std::memcpy(&id, ptr, 4);
        ptr += 4;

        float px, py, pz;
        std::memcpy(&px, ptr, 4);
        ptr += 4;
        std::memcpy(&py, ptr, 4);
        ptr += 4;
        std::memcpy(&pz, ptr, 4);
        ptr += 4;

        float rx, ry, rz, rw;
        std::memcpy(&rx, ptr, 4);
        ptr += 4;
        std::memcpy(&ry, ptr, 4);
        ptr += 4;
        std::memcpy(&rz, ptr, 4);
        ptr += 4;
        std::memcpy(&rw, ptr, 4);
        ptr += 4;

        float lvx, lvy, lvz;
        std::memcpy(&lvx, ptr, 4);
        ptr += 4;
        std::memcpy(&lvy, ptr, 4);
        ptr += 4;
        std::memcpy(&lvz, ptr, 4);
        ptr += 4;

        float avx, avy, avz;
        std::memcpy(&avx, ptr, 4);
        ptr += 4;
        std::memcpy(&avy, ptr, 4);
        ptr += 4;
        std::memcpy(&avz, ptr, 4);
        ptr += 4;

        uint8_t active = *ptr++;

        // Find matching body and restore state
        JPH::BodyID bodyID(id);
        if (bodyInterface.IsAdded(bodyID))
        {
            bodyInterface.SetPositionAndRotation(bodyID, JPH::RVec3(px, py, pz), JPH::Quat(rx, ry, rz, rw),
                                                 active ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            bodyInterface.SetLinearVelocity(bodyID, JPH::Vec3(lvx, lvy, lvz));
            bodyInterface.SetAngularVelocity(bodyID, JPH::Vec3(avx, avy, avz));
        }
    }

    return true;
}

// ============================================================================
// GROUP FILTER TABLE
// ============================================================================

uint32_t PhysicsSystem::CreateGroupFilterTable(uint32_t numSubGroups)
{
    auto* tableRef =
        new JPH::Ref<JPH::GroupFilterTable>(new JPH::GroupFilterTable(numSubGroups)); // Cleaned up in Shutdown()
    m_groupFilterTables.push_back(tableRef);
    return static_cast<uint32_t>(m_groupFilterTables.size()); // 1-based ID
}

void PhysicsSystem::DisableGroupCollision(uint32_t filterID, uint32_t subGroup1, uint32_t subGroup2)
{
    if (filterID == 0 || filterID > m_groupFilterTables.size())
        return;
    auto* tableRef = static_cast<JPH::Ref<JPH::GroupFilterTable>*>(m_groupFilterTables[filterID - 1]);
    (*tableRef)->DisableCollision(subGroup1, subGroup2);
}

void PhysicsSystem::EnableGroupCollision(uint32_t filterID, uint32_t subGroup1, uint32_t subGroup2)
{
    if (filterID == 0 || filterID > m_groupFilterTables.size())
        return;
    auto* tableRef = static_cast<JPH::Ref<JPH::GroupFilterTable>*>(m_groupFilterTables[filterID - 1]);
    (*tableRef)->EnableCollision(subGroup1, subGroup2);
}

// ============================================================================
// MUTABLE COMPOUND SHAPE
// ============================================================================

std::shared_ptr<PhysicsBody> PhysicsSystem::CreateMutableCompoundBody(const PhysicsBodyDesc& desc,
                                                                      const std::vector<MutableSubShapeDesc>& subShapes)
{
    if (!m_joltSystem)
        return nullptr;

    // Build the MutableCompoundShape from sub-shapes
    JPH::MutableCompoundShapeSettings compoundSettings;
    for (const auto& sub : subShapes)
    {
        void* shapePtr = CreateCollisionShape(sub.shape);
        if (!shapePtr)
            continue;
        auto* shapeRef = static_cast<JPH::ShapeRefC*>(shapePtr);

        float pitch = sub.rotation.x * (3.14159265f / 180.0f);
        float yaw = sub.rotation.y * (3.14159265f / 180.0f);
        float roll = sub.rotation.z * (3.14159265f / 180.0f);
        JPH::Quat rot = JPH::Quat::sEulerAngles(JPH::Vec3(pitch, yaw, roll));

        compoundSettings.AddShape(JPH::Vec3(sub.position.x, sub.position.y, sub.position.z), rot, shapeRef->GetPtr());
    }

    auto compoundResult = compoundSettings.Create();
    if (compoundResult.HasError())
        return nullptr;

    // Determine body layer and motion type
    JPH::EMotionType motionType = JPH::EMotionType::Dynamic;
    JPH::ObjectLayer layer = 1; // MOVING
    if (desc.type == PhysicsBodyType::Static)
    {
        motionType = JPH::EMotionType::Static;
        layer = 0; // NON_MOVING
    }
    else if (desc.type == PhysicsBodyType::Kinematic || desc.isKinematic)
    {
        motionType = JPH::EMotionType::Kinematic;
    }

    float pitch = desc.rotation.x * (3.14159265f / 180.0f);
    float yaw = desc.rotation.y * (3.14159265f / 180.0f);
    float roll = desc.rotation.z * (3.14159265f / 180.0f);

    JPH::BodyCreationSettings bodySettings(compoundResult.Get(),
                                           JPH::RVec3(desc.position.x, desc.position.y, desc.position.z),
                                           JPH::Quat::sEulerAngles(JPH::Vec3(pitch, yaw, roll)), motionType, layer);
    if (motionType == JPH::EMotionType::Dynamic)
    {
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = desc.mass;
    }

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(bodySettings, JPH::EActivation::Activate);

    auto body = std::make_shared<PhysicsBody>(desc, bodyID.GetIndexAndSequenceNumber());

    // Mirror CreateBody(): seed the wrapper's layer bits and register the wrapper as
    // Jolt user data so filtered queries and collision callbacks can classify this body.
    body->SetCollisionGroup(desc.collisionGroup);
    body->SetCollisionMask(desc.collisionMask);
    bodyInterface.SetUserData(bodyID, reinterpret_cast<uint64_t>(body.get()));

    m_bodies.push_back(body);
    m_bodyIDMap[bodyID.GetIndexAndSequenceNumber()] = body.get();
    if (!desc.name.empty())
        m_namedBodies[desc.name] = body;

    return body;
}

uint32_t PhysicsSystem::AddSubShape(std::shared_ptr<PhysicsBody> body, const MutableSubShapeDesc& subShape)
{
    if (!m_joltSystem || !body)
        return UINT32_MAX;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());
    if (!bodyInterface.IsAdded(bodyID))
        return UINT32_MAX;

    JPH::Body* joltBody = const_cast<JPH::Body*>(m_joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!joltBody)
        return UINT32_MAX;

    auto* compound =
        const_cast<JPH::MutableCompoundShape*>(static_cast<const JPH::MutableCompoundShape*>(joltBody->GetShape()));
    if (!compound)
        return UINT32_MAX;

    void* shapePtr = CreateCollisionShape(subShape.shape);
    if (!shapePtr)
        return UINT32_MAX;

    auto* shapeRef = static_cast<JPH::ShapeRefC*>(shapePtr);

    float pitch = subShape.rotation.x * (3.14159265f / 180.0f);
    float yaw = subShape.rotation.y * (3.14159265f / 180.0f);
    float roll = subShape.rotation.z * (3.14159265f / 180.0f);
    JPH::Quat rot = JPH::Quat::sEulerAngles(JPH::Vec3(pitch, yaw, roll));

    uint32_t index = compound->AddShape(JPH::Vec3(subShape.position.x, subShape.position.y, subShape.position.z), rot,
                                        shapeRef->GetPtr(), subShape.userData);

    compound->AdjustCenterOfMass();
    bodyInterface.NotifyShapeChanged(bodyID, JPH::Vec3::sZero(), true, JPH::EActivation::Activate);

    return index;
}

void PhysicsSystem::RemoveSubShape(std::shared_ptr<PhysicsBody> body, uint32_t index)
{
    if (!m_joltSystem || !body)
        return;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());
    if (!bodyInterface.IsAdded(bodyID))
        return;

    JPH::Body* joltBody = const_cast<JPH::Body*>(m_joltSystem->GetBodyLockInterface().TryGetBody(bodyID));
    if (!joltBody)
        return;

    auto* compound =
        const_cast<JPH::MutableCompoundShape*>(static_cast<const JPH::MutableCompoundShape*>(joltBody->GetShape()));
    if (!compound)
        return;

    compound->RemoveShape(index);
    compound->AdjustCenterOfMass();
    bodyInterface.NotifyShapeChanged(bodyID, JPH::Vec3::sZero(), true, JPH::EActivation::Activate);
}

// ============================================================================
// OFFSET CENTER OF MASS
// ============================================================================

void PhysicsSystem::SetCenterOfMassOffset(std::shared_ptr<PhysicsBody> body, const XMFLOAT3& offset)
{
    if (!m_joltSystem || !body)
        return;

    auto& bodyInterface = m_joltSystem->GetBodyInterface();
    JPH::BodyID bodyID(body->GetJoltBodyID());
    if (!bodyInterface.IsAdded(bodyID))
        return;

    // Get current shape and wrap it with OffsetCenterOfMassShape
    JPH::RefConst<JPH::Shape> currentShape = bodyInterface.GetShape(bodyID);

    JPH::OffsetCenterOfMassShapeSettings settings(JPH::Vec3(offset.x, offset.y, offset.z), currentShape.GetPtr());
    auto result = settings.Create();
    if (result.HasError())
        return;

    bodyInterface.SetShape(bodyID, result.Get(), true, JPH::EActivation::Activate);
}
