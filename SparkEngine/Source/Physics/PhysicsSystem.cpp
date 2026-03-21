#include "../Core/Platform.h"
/**
 * @file PhysicsSystem.cpp
 * @brief Core PhysicsSystem lifecycle, simulation loop, and collision processing
 * @author Spark Engine Team
 * @date 2025
 *
 * Body/constraint management and spatial queries live in PhysicsSystemQueries.cpp.
 * Shape creation and Bullet conversion helpers live in PhysicsShapeFactory.cpp.
 * PhysicsBody and PhysicsConstraint methods live in PhysicsBodyImpl.cpp.
 */

#include "PhysicsSystem.h"
#include "Engine/Events/EventSystem.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#include <algorithm>
#include <chrono>

using namespace DirectX;

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

    // Enable ghost object pair callback for overlap tests (owned by us, freed in Shutdown)
    m_ghostPairCallback = new btGhostPairCallback();
    m_broadphase->getOverlappingPairCache()->setInternalGhostPairCallback(m_ghostPairCallback);

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

    // Delete cached collision shapes (must happen before deleting triangle meshes
    // since btBvhTriangleMeshShape references the btTriangleMesh data)
    for (auto& [hash, shape] : m_shapeCache)
    {
        delete shape;
    }
    m_shapeCache.clear();

    // Delete triangle mesh data used by btBvhTriangleMeshShape instances
    for (auto* triMesh : m_triangleMeshes)
    {
        delete triMesh;
    }
    m_triangleMeshes.clear();

    // Delete Bullet world components in reverse order
    delete m_dynamicsWorld;
    m_dynamicsWorld = nullptr;

    delete m_solver;
    m_solver = nullptr;

    // Ghost pair callback must be deleted before the broadphase that references it
    delete m_ghostPairCallback;
    m_ghostPairCallback = nullptr;

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

    std::vector<std::pair<PhysicsBody*, PhysicsBody*>> currentTriggerPairs;
    DispatchCollisionCallbacks(currentTriggerPairs);
    UpdateTriggerExitEvents(currentTriggerPairs);

    // Swap the current pairs into the tracking set for the next frame
    m_activeTriggerPairs = std::move(currentTriggerPairs);
}

void PhysicsSystem::DispatchCollisionCallbacks(std::vector<std::pair<PhysicsBody*, PhysicsBody*>>& outTriggerPairs)
{
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
            outTriggerPairs.push_back({first, second});

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

                    if (m_eventBus)
                    {
                        auto idA = bodyA->GetEntityID();
                        auto idB = bodyB->GetEntityID();
                        m_eventBus->Publish(Spark::TriggerEnterEvent{idA, idB});
                    }
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

                if (m_eventBus)
                {
                    auto idA = bodyA->GetEntityID();
                    auto idB = bodyB->GetEntityID();
                    m_eventBus->Publish(Spark::CollisionEvent{idA, idB, pt.getAppliedImpulse()});
                }
            }
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
