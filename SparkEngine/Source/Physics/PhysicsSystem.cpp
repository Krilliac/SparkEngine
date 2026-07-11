#include "PhysicsSystem.h"
#include "../Core/Platform.h"
/**
 * @file PhysicsSystem.cpp
 * @brief Core PhysicsSystem lifecycle, simulation loop, and collision processing
 * @author Spark Engine Team
 * @date 2025
 *
 * @note MEMORY INTEGRITY GUARDS: NOT INCLUDED in this file.
 * Reason: Physics simulation runs every frame at high frequency. Adding branch
 * guards to the inner step/collision loop would add unacceptable overhead.
 * Server-authoritative physics validation (if needed) should be done at the
 * network replication layer, not inside the simulation tick.
 *
 * Body lifecycle, debug rendering, materials, serialization live in PhysicsSystemQueries.cpp.
 * Constraint creation lives in PhysicsConstraints.cpp.
 * Constraint motor control lives in PhysicsMotorControl.cpp.
 * Raycasting, overlap, and sweep queries live in PhysicsSpatialQueries.cpp.
 * Shape creation and Jolt conversion helpers live in PhysicsShapeFactory.cpp.
 * PhysicsBody and PhysicsConstraint methods live in PhysicsBodyImpl.cpp.
 */

#include "Engine/Events/EventSystem.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"

// Jolt includes
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>

#include "Utils/DebugHookManager.h"

#include <algorithm>
#include <chrono>
#include <thread>

JPH_SUPPRESS_WARNINGS
#include "JoltWarningRestore.h"

using namespace DirectX;

// ============================================================================
// JOLT OBJECT LAYERS AND BROADPHASE LAYERS
// ============================================================================

namespace SparkPhysicsLayers
{
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING = 1;
    static constexpr JPH::ObjectLayer TRIGGER = 2;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 3;
} // namespace SparkPhysicsLayers

namespace SparkBroadPhaseLayers
{
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr uint32_t NUM_LAYERS = 2;
} // namespace SparkBroadPhaseLayers

// BroadPhaseLayerInterface implementation
class SparkBPLayerInterface final : public JPH::BroadPhaseLayerInterface
{
  public:
    SparkBPLayerInterface()
    {
        m_objectToBroadPhase[SparkPhysicsLayers::NON_MOVING] = SparkBroadPhaseLayers::NON_MOVING;
        m_objectToBroadPhase[SparkPhysicsLayers::MOVING] = SparkBroadPhaseLayers::MOVING;
        m_objectToBroadPhase[SparkPhysicsLayers::TRIGGER] = SparkBroadPhaseLayers::MOVING;
    }

    uint32_t GetNumBroadPhaseLayers() const override { return SparkBroadPhaseLayers::NUM_LAYERS; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
    {
        return m_objectToBroadPhase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
    {
        switch (static_cast<JPH::BroadPhaseLayer::Type>(inLayer))
        {
        case static_cast<JPH::BroadPhaseLayer::Type>(SparkBroadPhaseLayers::NON_MOVING):
            return "NON_MOVING";
        case static_cast<JPH::BroadPhaseLayer::Type>(SparkBroadPhaseLayers::MOVING):
            return "MOVING";
        default:
            return "INVALID";
        }
    }
#endif

  private:
    JPH::BroadPhaseLayer m_objectToBroadPhase[SparkPhysicsLayers::NUM_LAYERS];
};

// ObjectVsBroadPhaseLayerFilter implementation
class SparkObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
  public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
    {
        switch (inLayer1)
        {
        case SparkPhysicsLayers::NON_MOVING:
            return inLayer2 == SparkBroadPhaseLayers::MOVING;
        case SparkPhysicsLayers::MOVING:
        case SparkPhysicsLayers::TRIGGER:
            return true;
        default:
            return false;
        }
    }
};

// ObjectLayerPairFilter implementation
class SparkObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
  public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override
    {
        switch (inLayer1)
        {
        case SparkPhysicsLayers::NON_MOVING:
            return inLayer2 == SparkPhysicsLayers::MOVING || inLayer2 == SparkPhysicsLayers::TRIGGER;
        case SparkPhysicsLayers::MOVING:
        case SparkPhysicsLayers::TRIGGER:
            return true;
        default:
            return false;
        }
    }
};

// Contact listener for collision callbacks — dispatches to SparkEngine's callback system
class SparkContactListener final : public JPH::ContactListener
{
  public:
    ::PhysicsSystem* m_physicsSystem = nullptr;

    // Store pending contacts for dispatch after the simulation step
    struct PendingContact
    {
        uint32_t bodyID1;
        uint32_t bodyID2;
        XMFLOAT3 contactPoint;
        XMFLOAT3 contactNormal;
        float penetrationDepth;
        float combinedFriction;
        float combinedRestitution;
        bool isSensor;
    };
    std::vector<PendingContact> m_pendingContacts;
    std::mutex m_contactMutex;

    JPH::ValidateResult OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2,
                                          JPH::RVec3Arg /*inBaseOffset*/,
                                          const JPH::CollideShapeResult& /*inCollisionResult*/) override
    {
        // Apply collision group/mask filtering from PhysicsBody wrappers
        if (m_physicsSystem)
        {
            PhysicsBody* bodyA = m_physicsSystem->FindBodyByJoltID(inBody1.GetID().GetIndexAndSequenceNumber());
            PhysicsBody* bodyB = m_physicsSystem->FindBodyByJoltID(inBody2.GetID().GetIndexAndSequenceNumber());
            if (bodyA && bodyB)
            {
                uint16_t groupA = bodyA->GetCollisionGroup();
                uint16_t maskA = bodyA->GetCollisionMask();
                uint16_t groupB = bodyB->GetCollisionGroup();
                uint16_t maskB = bodyB->GetCollisionMask();
                if ((groupA & maskB) == 0 || (groupB & maskA) == 0)
                {
                    return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
                }
            }
        }

        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    // Surface velocity lookup — set by PhysicsSystem::SetSurfaceVelocity()
    std::unordered_map<uint32_t, XMFLOAT3>* m_surfaceVelocities = nullptr;
    std::mutex* m_surfaceVelocityMutex = nullptr;

    void ApplySurfaceVelocity(const JPH::Body& inBody1, const JPH::Body& inBody2,
                              JPH::ContactSettings& ioSettings) const
    {
        if (!m_surfaceVelocities || !m_surfaceVelocityMutex)
            return;

        std::lock_guard<std::mutex> lock(*m_surfaceVelocityMutex);

        auto it1 = m_surfaceVelocities->find(inBody1.GetID().GetIndexAndSequenceNumber());
        if (it1 != m_surfaceVelocities->end())
        {
            ioSettings.mRelativeLinearSurfaceVelocity += JPH::Vec3(it1->second.x, it1->second.y, it1->second.z);
        }

        auto it2 = m_surfaceVelocities->find(inBody2.GetID().GetIndexAndSequenceNumber());
        if (it2 != m_surfaceVelocities->end())
        {
            ioSettings.mRelativeLinearSurfaceVelocity -= JPH::Vec3(it2->second.x, it2->second.y, it2->second.z);
        }
    }

    void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold,
                        JPH::ContactSettings& ioSettings) override
    {
        // Apply surface velocity (conveyor belts, moving walkways)
        ApplySurfaceVelocity(inBody1, inBody2, ioSettings);

        PendingContact contact;
        contact.bodyID1 = inBody1.GetID().GetIndexAndSequenceNumber();
        contact.bodyID2 = inBody2.GetID().GetIndexAndSequenceNumber();

        // Get the first contact point from the manifold
        JPH::RVec3 worldPoint = inManifold.GetWorldSpaceContactPointOn1(0);
        contact.contactPoint = XMFLOAT3(static_cast<float>(worldPoint.GetX()), static_cast<float>(worldPoint.GetY()),
                                        static_cast<float>(worldPoint.GetZ()));
        JPH::Vec3 normal = inManifold.mWorldSpaceNormal;
        contact.contactNormal = XMFLOAT3(normal.GetX(), normal.GetY(), normal.GetZ());
        contact.penetrationDepth = inManifold.mPenetrationDepth;
        contact.combinedFriction = ioSettings.mCombinedFriction;
        contact.combinedRestitution = ioSettings.mCombinedRestitution;
        contact.isSensor = inBody1.IsSensor() || inBody2.IsSensor();

        std::lock_guard<std::mutex> lock(m_contactMutex);
        m_pendingContacts.push_back(contact);
    }

    void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold,
                            JPH::ContactSettings& ioSettings) override
    {
        // Apply surface velocity on persistent contacts too
        ApplySurfaceVelocity(inBody1, inBody2, ioSettings);

        // Persistent contacts for trigger tracking
        if (inBody1.IsSensor() || inBody2.IsSensor())
        {
            PendingContact contact;
            contact.bodyID1 = inBody1.GetID().GetIndexAndSequenceNumber();
            contact.bodyID2 = inBody2.GetID().GetIndexAndSequenceNumber();
            contact.isSensor = true;
            contact.penetrationDepth = inManifold.mPenetrationDepth;

            JPH::RVec3 worldPoint = inManifold.GetWorldSpaceContactPointOn1(0);
            contact.contactPoint =
                XMFLOAT3(static_cast<float>(worldPoint.GetX()), static_cast<float>(worldPoint.GetY()),
                         static_cast<float>(worldPoint.GetZ()));
            JPH::Vec3 normal = inManifold.mWorldSpaceNormal;
            contact.contactNormal = XMFLOAT3(normal.GetX(), normal.GetY(), normal.GetZ());

            std::lock_guard<std::mutex> lock(m_contactMutex);
            m_pendingContacts.push_back(contact);
        }
    }

    std::vector<PendingContact> DrainContacts()
    {
        std::lock_guard<std::mutex> lock(m_contactMutex);
        std::vector<PendingContact> result;
        result.swap(m_pendingContacts);
        return result;
    }
};

// Body activation listener — tracks sleep/wake state changes
class SparkBodyActivationListener final : public JPH::BodyActivationListener
{
  public:
    void OnBodyActivated(const JPH::BodyID& /*inBodyID*/, uint64_t /*inBodyUserData*/) override
    {
        // Bodies waking up — can be used for LOD/optimization
    }

    void OnBodyDeactivated(const JPH::BodyID& /*inBodyID*/, uint64_t /*inBodyUserData*/) override
    {
        // Bodies going to sleep — can be used to stop updating transforms
    }
};

// ============================================================================
// JOLT TRACE/ASSERT CALLBACKS
// ============================================================================

static void JoltTraceImpl(const char* inFMT, ...)
{
    va_list list;
    va_start(list, inFMT);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), inFMT, list);
    va_end(list);

    SPARK_LOG_INFO(Spark::LogCategory::Physics, "Jolt: %s", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool JoltAssertFailed(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine)
{
    SPARK_LOG_ERROR(Spark::LogCategory::Physics, "Jolt assert failed: %s : %s (%s:%u)", inExpression,
                    inMessage ? inMessage : "", inFile, inLine);
    return true; // Break into debugger
}
#endif

// ============================================================================
// PHYSICS SYSTEM IMPLEMENTATION
// ============================================================================

PhysicsSystem::PhysicsSystem() {}

PhysicsSystem::~PhysicsSystem()
{
    Shutdown();
}

void PhysicsSystem::EnsureImageRuntime()
{
    // Module DLLs statically link SparkEngineLib, so every DLL carries its OWN
    // copy of Jolt's per-image globals: the Allocate/Free function pointers,
    // Factory::sInstance, and the CollisionDispatch function tables filled in
    // by RegisterTypes(). The exe registers ITS copies in Initialize(), but a
    // non-virtual PhysicsSystem method called from module code executes the
    // MODULE image's copy of that method — which reads the module's
    // uninitialized globals (first symptom: null-call AV inside
    // JPH::...::operator new during TFWorldCollision::Build). Idempotent:
    // call from any image before that image creates shapes or runs queries.
    // Virtual dispatch on exe-created Jolt objects is unaffected (their
    // vtables point into the exe image); only per-image statics need this.
    if (JPH::Factory::sInstance == nullptr)
    {
        JPH::RegisterDefaultAllocator();
        auto factory = std::make_unique<JPH::Factory>();
        JPH::Factory::sInstance = factory.release(); // per-image; freed at process exit
        JPH::RegisterTypes();
    }
}

HRESULT PhysicsSystem::Initialize()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "Physics", 0.0);
    SPARK_LOG_INFO(Spark::LogCategory::Physics, "PhysicsSystem initializing (Jolt Physics)");

    // Initialize default material
    m_defaultMaterial.friction = 0.5f;
    m_defaultMaterial.restitution = 0.1f;
    m_defaultMaterial.linearDamping = 0.1f;
    m_defaultMaterial.angularDamping = 0.1f;
    m_defaultMaterial.density = 1.0f;
    m_defaultMaterial.name = "Default";

    // Initialize metrics
    m_metrics = {};

    // Re-arm the teardown-guard WARN latch (see RemoveBody) for this world's lifetime
    m_warnedRemoveAfterTeardown = false;

    // Register Jolt types and install callbacks (per-image guard — see
    // EnsureImageRuntime below for why this must also run inside module DLLs)
    EnsureImageRuntime();
    JPH::Trace = JoltTraceImpl;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = JoltAssertFailed;
#endif

    // Create temp allocator (16 MB — enough for complex scenes with many contacts)
    m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);

    // Create multi-threaded job system — use all available cores minus one (for game thread)
    // Jolt scales nearly linearly: 4.9x speedup at 8 threads, 5.7x at 16 (with SMT)
    int numThreads = static_cast<int>(std::thread::hardware_concurrency()) - 1;
    if (numThreads < 1)
        numThreads = 0; // 0 = run on calling thread only
    m_jobSystem =
        std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, numThreads);
    SPARK_LOG_INFO(Spark::LogCategory::Physics, "Jolt job system: %d worker threads", numThreads);

    // Create broadphase layer interface and filters
    m_broadPhaseLayerInterface = std::make_unique<SparkBPLayerInterface>();
    m_objectVsBroadphaseFilter = std::make_unique<SparkObjectVsBroadPhaseFilter>();
    m_objectLayerPairFilter = std::make_unique<SparkObjectLayerPairFilter>();

    // Create Jolt physics system
    const uint32_t cMaxBodies = 65536;
    const uint32_t cNumBodyMutexes = 0; // Auto-detect
    const uint32_t cMaxBodyPairs = 65536;
    const uint32_t cMaxContactConstraints = 10240;

    m_joltSystem = std::make_unique<JPH::PhysicsSystem>();
    m_joltSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, *m_broadPhaseLayerInterface,
                       *m_objectVsBroadphaseFilter, *m_objectLayerPairFilter);

    // Set default gravity
    m_joltSystem->SetGravity(JPH::Vec3(0, -9.8f, 0));

    // Install contact listener
    auto contactListener = std::make_unique<SparkContactListener>();
    contactListener->m_physicsSystem = this;
    contactListener->m_surfaceVelocities = &m_surfaceVelocities;
    contactListener->m_surfaceVelocityMutex = &m_surfaceVelocityMutex;
    m_contactListener = std::move(contactListener);
    m_joltSystem->SetContactListener(static_cast<SparkContactListener*>(m_contactListener.get()));

    // Install body activation listener
    m_bodyActivationListener = std::make_unique<SparkBodyActivationListener>();
    m_joltSystem->SetBodyActivationListener(static_cast<SparkBodyActivationListener*>(m_bodyActivationListener.get()));

    Spark::SimpleConsole::GetInstance().LogSuccess("PhysicsSystem initialized successfully (Jolt Physics)");
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "Physics", 0.0);
    return S_OK;
}

void PhysicsSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    if (!m_joltSystem && m_bodies.empty() && m_constraints.empty())
        return;

    SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "Physics", 0.0);
    SPARK_LOG_INFO(Spark::LogCategory::Physics, "PhysicsSystem shutting down");

    // Remove all constraints
    if (m_joltSystem)
    {
        for (auto& constraint : m_constraints)
        {
            if (constraint && constraint->GetJoltConstraint())
            {
                m_joltSystem->RemoveConstraint(constraint->GetJoltConstraint());
            }
        }
    }
    m_constraints.clear();

    // Remove all bodies from Jolt
    if (m_joltSystem)
    {
        auto& bodyInterface = m_joltSystem->GetBodyInterface();
        for (auto& body : m_bodies)
        {
            if (body)
            {
                JPH::BodyID bodyID(body->GetJoltBodyID());
                bodyInterface.RemoveBody(bodyID);
                bodyInterface.DestroyBody(bodyID);
            }
        }
    }
    m_bodies.clear();
    m_namedBodies.clear();
    m_bodyIDMap.clear();

    // Delete heap-allocated ShapeRefC entries before clearing the cache
    for (auto& [hash, shapePtr] : m_shapeCache)
    {
        delete static_cast<JPH::ShapeRefC*>(shapePtr);
    }
    m_shapeCache.clear();

    // Delete heap-allocated group filter table entries
    for (auto* tablePtr : m_groupFilterTables)
    {
        delete static_cast<JPH::Ref<JPH::GroupFilterTable>*>(tablePtr);
    }
    m_groupFilterTables.clear();

    // Null out contact listener pointers before destroying Jolt (prevents dangling access
    // from physics thread callbacks during shutdown)
    if (m_contactListener)
    {
        auto* cl = static_cast<SparkContactListener*>(m_contactListener.get());
        std::lock_guard<std::mutex> lock(m_surfaceVelocityMutex);
        cl->m_surfaceVelocities = nullptr;
        cl->m_surfaceVelocityMutex = nullptr;
    }

    // Destroy Jolt systems in reverse order
    m_joltSystem.reset();
    m_contactListener.reset();
    m_bodyActivationListener.reset();
    m_objectLayerPairFilter.reset();
    m_objectVsBroadphaseFilter.reset();
    m_broadPhaseLayerInterface.reset();
    m_jobSystem.reset();
    m_tempAllocator.reset();

    // Cleanup Jolt factory — take ownership back from the global, then let unique_ptr delete
    JPH::UnregisterTypes();
    std::unique_ptr<JPH::Factory> factoryCleanup(JPH::Factory::sInstance);
    JPH::Factory::sInstance = nullptr;

    Spark::SimpleConsole::GetInstance().LogInfo("PhysicsSystem shutdown complete");
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Physics", 0.0);
}

void PhysicsSystem::Update(float deltaTime)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Physics", 0.0);
    SPARK_WARN_IF(Spark::LogCategory::Physics, deltaTime < 0.0f,
                  "PhysicsSystem::Update called with negative deltaTime");
    if (m_paused || deltaTime <= 0.0f)
    {
        return;
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    if (m_joltSystem)
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

                // Step the Jolt simulation (1 collision step)
                m_joltSystem->Update(m_timeStep, 1, m_tempAllocator.get(), m_jobSystem.get());

                // Read back new state from Jolt
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
            // Determine number of steps based on deltaTime (guard against zero timestep)
            int numSteps = (m_timeStep > 0.0f) ? static_cast<int>(std::ceil(deltaTime / m_timeStep)) : 1;
            if (numSteps > m_maxSubsteps)
                numSteps = m_maxSubsteps;
            if (numSteps < 1)
                numSteps = 1;

            m_joltSystem->Update(deltaTime, numSteps, m_tempAllocator.get(), m_jobSystem.get());
        }
    }

    ProcessCollisions();

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.simulationTime = static_cast<float>(duration.count()) / 1000.0f;
    }

    SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Physics", static_cast<double>(duration.count()) / 1000.0);
    UpdateMetrics();
}

uint32_t PhysicsSystem::StepFixed(uint32_t stepCount, float interpolationAlpha)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);

    if (m_paused || !m_joltSystem)
    {
        return 0;
    }

    // Refresh the render interpolation alpha even on zero-tick frames so
    // interpolated transforms keep advancing between fixed ticks.
    m_interpolationAlpha = std::clamp(interpolationAlpha, 0.0f, 1.0f);

    if (stepCount == 0)
    {
        return 0;
    }

    // Safety clamp mirroring the caller-side accumulator cap (spiral-of-death guard).
    uint32_t steps = stepCount;
    if (m_maxSubsteps > 0 && steps > static_cast<uint32_t>(m_maxSubsteps))
    {
        steps = static_cast<uint32_t>(m_maxSubsteps);
    }

    SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Physics", 0.0);
    auto startTime = std::chrono::high_resolution_clock::now();

    for (uint32_t i = 0; i < steps; ++i)
    {
        // Snapshot current state as previous before stepping (for interpolation)
        for (auto& body : m_bodies)
        {
            if (body)
            {
                body->StoreCurrentState();
            }
        }

        m_joltSystem->Update(m_timeStep, 1, m_tempAllocator.get(), m_jobSystem.get());

        for (auto& body : m_bodies)
        {
            if (body)
            {
                body->UpdateCurrentState();
            }
        }
    }

    ProcessCollisions();

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.simulationTime = static_cast<float>(duration.count()) / 1000.0f;
        m_metrics.substeps = steps;
    }

    SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Physics", static_cast<double>(duration.count()) / 1000.0);
    UpdateMetrics();
    return steps;
}

void PhysicsSystem::OptimizeBroadPhase()
{
    if (m_joltSystem)
        m_joltSystem->OptimizeBroadPhase();
}

void PhysicsSystem::UpdateMetrics()
{
    if (!m_joltSystem)
        return;

    std::lock_guard<std::mutex> lock(m_metricsMutex);

    m_metrics.totalRigidBodies = static_cast<uint32_t>(m_bodies.size());
    m_metrics.activeConstraints = static_cast<uint32_t>(m_constraints.size());
    m_metrics.timeStep = m_timeStep;
    m_metrics.debugDrawEnabled = m_debugDrawEnabled;

    // Count active rigid bodies
    m_metrics.activeRigidBodies = static_cast<uint32_t>(
        std::count_if(m_bodies.begin(), m_bodies.end(), [](const auto& body) { return body && body->IsActive(); }));

    // Jolt body stats
    m_metrics.collisionPairs = m_joltSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody);
    m_metrics.broadphaseProxies = static_cast<uint32_t>(m_bodies.size());

    m_metrics.substeps = static_cast<uint32_t>(m_maxSubsteps);
}

void PhysicsSystem::ProcessCollisions()
{
    if (!m_joltSystem)
        return;

    std::vector<std::pair<PhysicsBody*, PhysicsBody*>> currentTriggerPairs;
    currentTriggerPairs.reserve(m_activeTriggerPairs.size());
    DispatchCollisionCallbacks(currentTriggerPairs);
    UpdateTriggerExitEvents(currentTriggerPairs);
    m_activeTriggerPairs = std::move(currentTriggerPairs);
}

void PhysicsSystem::DispatchCollisionCallbacks(std::vector<std::pair<PhysicsBody*, PhysicsBody*>>& outTriggerPairs)
{
    // Drain contacts collected by SparkContactListener during the simulation step
    auto* listener = static_cast<SparkContactListener*>(m_contactListener.get());
    if (!listener)
        return;

    auto contacts = listener->DrainContacts();

    for (const auto& contact : contacts)
    {
        PhysicsBody* bodyA = FindBodyByJoltID(contact.bodyID1);
        PhysicsBody* bodyB = FindBodyByJoltID(contact.bodyID2);
        if (!bodyA || !bodyB)
            continue;

        // Handle trigger (sensor) contacts
        if (contact.isSensor)
        {
            PhysicsBody* first = (bodyA < bodyB) ? bodyA : bodyB;
            PhysicsBody* second = (bodyA < bodyB) ? bodyB : bodyA;
            outTriggerPairs.push_back({first, second});

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

                    if (m_eventBus)
                    {
                        m_eventBus->Publish(Spark::TriggerEnterEvent{bodyA->GetEntityID(), bodyB->GetEntityID()});
                    }
                }
            }
            continue;
        }

        // Fire collision callback for non-trigger contacts
        if (m_collisionCallback)
        {
            ContactInfo info;
            info.bodyA = bodyA;
            info.bodyB = bodyB;
            info.contactPoint = contact.contactPoint;
            info.contactNormal = contact.contactNormal;
            info.penetrationDepth = contact.penetrationDepth;
            info.appliedImpulse = 0.0f; // Jolt doesn't expose applied impulse in contact listener
            info.entityIdA = bodyA->GetEntityID();
            info.entityIdB = bodyB->GetEntityID();

            m_collisionCallback(info);
        }

        if (m_eventBus)
        {
            m_eventBus->Publish(Spark::CollisionEvent{bodyA->GetEntityID(), bodyB->GetEntityID(), 0.0f});
        }
    }
}

void PhysicsSystem::UpdateTriggerExitEvents(
    const std::vector<std::pair<PhysicsBody*, PhysicsBody*>>& currentTriggerPairs)
{
    if (!m_triggerCallback)
        return;

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

            if (m_eventBus)
            {
                auto idA = prev.first->GetEntityID();
                auto idB = prev.second->GetEntityID();
                m_eventBus->Publish(Spark::TriggerExitEvent{idA, idB});
            }
        }
    }
}

void PhysicsSystem::SetGravity(const XMFLOAT3& gravity)
{
    if (!m_joltSystem)
        return;
    m_joltSystem->SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
}

XMFLOAT3 PhysicsSystem::GetGravity() const
{
    if (!m_joltSystem)
        return {0.0f, -9.8f, 0.0f};
    JPH::Vec3 g = m_joltSystem->GetGravity();
    return XMFLOAT3(g.GetX(), g.GetY(), g.GetZ());
}

void PhysicsSystem::SetDeterministicSimulation(bool enabled)
{
    m_deterministicMode = enabled;
    if (m_joltSystem)
    {
        JPH::PhysicsSettings settings = m_joltSystem->GetPhysicsSettings();
        settings.mDeterministicSimulation = enabled;
        m_joltSystem->SetPhysicsSettings(settings);
    }
}

PhysicsBody* PhysicsSystem::FindBodyByJoltID(uint32_t joltBodyID) const
{
    auto it = m_bodyIDMap.find(joltBodyID);
    return (it != m_bodyIDMap.end()) ? it->second : nullptr;
}
