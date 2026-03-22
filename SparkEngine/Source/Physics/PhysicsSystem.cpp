#include "../Core/Platform.h"
/**
 * @file PhysicsSystem.cpp
 * @brief Core PhysicsSystem lifecycle, simulation loop, and collision processing
 * @author Spark Engine Team
 * @date 2025
 *
 * Body/constraint management and spatial queries live in PhysicsSystemQueries.cpp.
 * Shape creation and Jolt conversion helpers live in PhysicsShapeFactory.cpp.
 * PhysicsBody and PhysicsConstraint methods live in PhysicsBodyImpl.cpp.
 */

#include "PhysicsSystem.h"
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

#include <algorithm>
#include <chrono>

JPH_SUPPRESS_WARNINGS

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

// Contact listener for collision callbacks
class SparkContactListener final : public JPH::ContactListener
{
  public:
    ::PhysicsSystem* m_physicsSystem = nullptr;

    JPH::ValidateResult OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2,
                                          JPH::RVec3Arg inBaseOffset,
                                          const JPH::CollideShapeResult& inCollisionResult) override
    {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold,
                        JPH::ContactSettings& ioSettings) override
    {
        // Collision events are processed in ProcessCollisions() via the step listener pattern
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

    SPARK_LOG_INFO(Spark::LogCategory::Physics, "Jolt: {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool JoltAssertFailed(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine)
{
    SPARK_LOG_ERROR(Spark::LogCategory::Physics, "Jolt assert failed: {} : {} ({}:{})", inExpression,
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

HRESULT PhysicsSystem::Initialize()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
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

    // Register Jolt types and install callbacks
    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTraceImpl;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = JoltAssertFailed;
#endif
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    // Create temp allocator (10 MB)
    m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

    // Create job system (use single-threaded for simplicity, matches Bullet's single-thread model)
    m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 0);

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
    m_contactListener = std::move(contactListener);
    m_joltSystem->SetContactListener(static_cast<SparkContactListener*>(m_contactListener.get()));

    Spark::SimpleConsole::GetInstance().LogSuccess("PhysicsSystem initialized successfully (Jolt Physics)");
    return S_OK;
}

void PhysicsSystem::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    if (!m_joltSystem && m_bodies.empty() && m_constraints.empty())
        return;

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

    // Clear shape cache
    m_shapeCache.clear();

    // Destroy Jolt systems in reverse order
    m_joltSystem.reset();
    m_contactListener.reset();
    m_bodyActivationListener.reset();
    m_objectLayerPairFilter.reset();
    m_objectVsBroadphaseFilter.reset();
    m_broadPhaseLayerInterface.reset();
    m_jobSystem.reset();
    m_tempAllocator.reset();

    // Cleanup Jolt factory
    JPH::UnregisterTypes();
    if (JPH::Factory::sInstance)
    {
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

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
            // Determine number of steps based on deltaTime
            int numSteps = static_cast<int>(std::ceil(deltaTime / m_timeStep));
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
        m_metrics.simulationTime = duration.count() / 1000.0f;
    }

    UpdateMetrics();
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

    // Walk active contact manifolds via body pairs
    auto& bodyInterface = m_joltSystem->GetBodyInterface();

    // For collision callbacks, iterate bodies and check for contacts
    if (m_collisionCallback || m_triggerCallback)
    {
        // Use Jolt's active contacts through the narrow phase
        // For each body pair with active contacts, dispatch callbacks
        for (auto& body : m_bodies)
        {
            if (!body || !body->IsActive())
                continue;

            bool isTrigger = body->IsTrigger();
            if (isTrigger && m_triggerCallback)
            {
                // Trigger events will be handled via contact listener in future improvement
                // For now, trigger state is tracked via body flags
            }
        }
    }

    DispatchCollisionCallbacks(currentTriggerPairs);
    UpdateTriggerExitEvents(currentTriggerPairs);

    m_activeTriggerPairs = std::move(currentTriggerPairs);
}

void PhysicsSystem::DispatchCollisionCallbacks(std::vector<std::pair<PhysicsBody*, PhysicsBody*>>& outTriggerPairs)
{
    // Jolt doesn't expose manifolds the same way Bullet does.
    // Contact events are handled through the ContactListener interface.
    // The SparkContactListener stores contacts for processing here.
    // For a minimal migration, collision callbacks fire through the listener.
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

PhysicsBody* PhysicsSystem::FindBodyByJoltID(uint32_t joltBodyID) const
{
    auto it = m_bodyIDMap.find(joltBodyID);
    return (it != m_bodyIDMap.end()) ? it->second : nullptr;
}
