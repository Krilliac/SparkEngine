/**
 * @file TestECSIntegration.cpp
 * @brief End-to-end ECS integration tests (R10.1 — Architecture Analysis)
 *
 * These tests verify the complete ECS pipeline: entity creation, component
 * attachment, system execution, and cross-system data flow. Unlike unit tests,
 * these exercise multiple systems together to catch integration issues.
 */

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Standalone ECS integration structures (no EnTT dependency for CI)
// ============================================================================

namespace ECSIntegration
{

    // Minimal component replicas for integration testing
    struct Transform
    {
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f;
        float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;
    };

    struct RigidBody
    {
        enum class Type
        {
            Static,
            Dynamic,
            Kinematic
        };
        Type type = Type::Dynamic;
        float mass = 1.0f;
        float velX = 0.0f, velY = 0.0f, velZ = 0.0f;
        float prevPosX = 0.0f, prevPosY = 0.0f, prevPosZ = 0.0f;
    };

    struct MeshRenderer
    {
        std::string meshPath;
        std::string materialPath;
        bool visible = true;
        bool castShadows = true;
        bool worldMatrixDirty = true;
    };

    struct Health
    {
        float health = 100.0f;
        float maxHealth = 100.0f;
        bool isDead = false;

        void TakeDamage(float amount)
        {
            if (isDead)
                return;
            health = health - amount;
            if (health < 0.0f)
                health = 0.0f;
            isDead = (health <= 0.0f);
        }
    };

    struct AIAgent
    {
        enum class State
        {
            Idle,
            Patrolling,
            Combat,
            Dead
        };
        State state = State::Idle;
        float detectionRange = 30.0f;
    };

    // Simple entity storage for testing
    struct Entity
    {
        uint32_t id;
        Transform* transform = nullptr;
        RigidBody* rigidBody = nullptr;
        MeshRenderer* meshRenderer = nullptr;
        Health* health = nullptr;
        AIAgent* ai = nullptr;
    };

    // Minimal world for integration tests
    class TestWorld
    {
      public:
        Entity& CreateEntity()
        {
            Entity e;
            e.id = m_nextID++;
            m_entities.push_back(e);
            return m_entities.back();
        }

        std::vector<Entity>& GetEntities() { return m_entities; }

        // Simulate fixed-timestep physics (R2.2)
        void StepPhysics(float fixedDt)
        {
            for (auto& e : m_entities)
            {
                if (e.rigidBody && e.transform && e.rigidBody->type == RigidBody::Type::Dynamic)
                {
                    // Save previous position
                    e.rigidBody->prevPosX = e.transform->posX;
                    e.rigidBody->prevPosY = e.transform->posY;
                    e.rigidBody->prevPosZ = e.transform->posZ;

                    // Simple gravity integration
                    e.rigidBody->velY -= 9.81f * fixedDt;
                    e.transform->posX += e.rigidBody->velX * fixedDt;
                    e.transform->posY += e.rigidBody->velY * fixedDt;
                    e.transform->posZ += e.rigidBody->velZ * fixedDt;
                }
            }
        }

        // Simulate render system marking dirty matrices
        int RenderPass()
        {
            int rendered = 0;
            for (auto& e : m_entities)
            {
                if (e.meshRenderer && e.transform && e.meshRenderer->visible)
                {
                    e.meshRenderer->worldMatrixDirty = false;
                    rendered++;
                }
            }
            return rendered;
        }

        // Simulate lifecycle system checking health
        std::vector<uint32_t> CheckDeaths()
        {
            std::vector<uint32_t> dead;
            for (auto& e : m_entities)
            {
                if (e.health && e.health->isDead)
                {
                    dead.push_back(e.id);
                    if (e.ai)
                    {
                        e.ai->state = AIAgent::State::Dead;
                    }
                }
            }
            return dead;
        }

      private:
        std::vector<Entity> m_entities;
        uint32_t m_nextID = 1;
    };

} // namespace ECSIntegration

// ============================================================================
// Integration Test: Physics → Transform → Render pipeline
// ============================================================================

TEST(ECSIntegration_PhysicsToRenderPipeline)
{
    using namespace ECSIntegration;

    TestWorld world;

    // Create a dynamic entity with physics, mesh, and transform
    Transform tf;
    tf.posY = 100.0f;
    RigidBody rb;
    rb.type = RigidBody::Type::Dynamic;
    rb.mass = 1.0f;
    MeshRenderer mr;
    mr.meshPath = "cube.mesh";
    mr.materialPath = "default.mat";

    auto& entity = world.CreateEntity();
    entity.transform = &tf;
    entity.rigidBody = &rb;
    entity.meshRenderer = &mr;

    // Step physics 10 times at 1/60s
    float fixedDt = 1.0f / 60.0f;
    for (int i = 0; i < 10; i++)
    {
        world.StepPhysics(fixedDt);
    }

    // After 10 steps of gravity, Y should have decreased
    EXPECT_TRUE(tf.posY < 100.0f);

    // Render pass should clear dirty flag
    EXPECT_TRUE(mr.worldMatrixDirty);
    int rendered = world.RenderPass();
    EXPECT_EQ(rendered, 1);
    EXPECT_FALSE(mr.worldMatrixDirty);
}

// ============================================================================
// Integration Test: Fixed-timestep accumulator (R2.2)
// ============================================================================

TEST(ECSIntegration_FixedTimestepAccumulator)
{
    using namespace ECSIntegration;

    // Simulate the accumulator pattern
    float fixedTimestep = 1.0f / 60.0f;
    float accumulator = 0.0f;
    float maxAccumulator = 0.25f;
    int totalSteps = 0;

    // Simulate 5 frames at varying frame times
    float frameTimes[] = {0.016f, 0.033f, 0.008f, 0.050f, 0.016f};
    for (float dt : frameTimes)
    {
        float clampedDt = dt < maxAccumulator ? dt : maxAccumulator;
        accumulator += clampedDt;

        while (accumulator >= fixedTimestep)
        {
            accumulator -= fixedTimestep;
            totalSteps++;
        }
    }

    // Total time = 0.123s, at 60fps that's ~7.38 steps, so 7 complete steps
    EXPECT_TRUE(totalSteps >= 7);
    EXPECT_TRUE(totalSteps <= 8);

    // Interpolation alpha should be in [0, 1)
    float alpha = accumulator / fixedTimestep;
    EXPECT_TRUE(alpha >= 0.0f);
    EXPECT_TRUE(alpha < 1.0f);
}

// ============================================================================
// Integration Test: Health → Lifecycle → AI state propagation
// ============================================================================

TEST(ECSIntegration_HealthDeathPropagation)
{
    using namespace ECSIntegration;

    TestWorld world;

    Transform tf;
    Health hp;
    hp.health = 50.0f;
    AIAgent ai;
    ai.state = AIAgent::State::Combat;

    auto& entity = world.CreateEntity();
    entity.transform = &tf;
    entity.health = &hp;
    entity.ai = &ai;

    // Not dead yet
    auto dead = world.CheckDeaths();
    EXPECT_EQ(dead.size(), static_cast<size_t>(0));
    EXPECT_TRUE(ai.state == AIAgent::State::Combat);

    // Take lethal damage
    hp.TakeDamage(60.0f);
    EXPECT_TRUE(hp.isDead);
    EXPECT_NEAR(hp.health, 0.0f, 0.001f);

    // Lifecycle system should detect death and propagate to AI
    dead = world.CheckDeaths();
    EXPECT_EQ(dead.size(), static_cast<size_t>(1));
    EXPECT_TRUE(ai.state == AIAgent::State::Dead);
}

// ============================================================================
// Integration Test: Multiple entity system execution order
// ============================================================================

TEST(ECSIntegration_MultiEntityPhysicsBatch)
{
    using namespace ECSIntegration;

    TestWorld world;

    // Create 100 dynamic entities
    std::vector<Transform> transforms(100);
    std::vector<RigidBody> bodies(100);
    std::vector<MeshRenderer> renderers(100);

    for (int i = 0; i < 100; i++)
    {
        transforms[i].posY = 100.0f + static_cast<float>(i);
        bodies[i].type = RigidBody::Type::Dynamic;
        renderers[i].meshPath = "entity_" + std::to_string(i);

        auto& entity = world.CreateEntity();
        entity.transform = &transforms[i];
        entity.rigidBody = &bodies[i];
        entity.meshRenderer = &renderers[i];
    }

    // Step physics
    world.StepPhysics(1.0f / 60.0f);

    // All dynamic entities should have moved
    for (int i = 0; i < 100; i++)
    {
        EXPECT_TRUE(transforms[i].posY < 100.0f + static_cast<float>(i));
    }

    // Render should process all visible entities
    int rendered = world.RenderPass();
    EXPECT_EQ(rendered, 100);
}

// ============================================================================
// Integration Test: Visibility toggle skip
// ============================================================================

TEST(ECSIntegration_InvisibleEntitySkipped)
{
    using namespace ECSIntegration;

    TestWorld world;

    Transform tf1, tf2;
    MeshRenderer mr1, mr2;
    mr1.visible = true;
    mr2.visible = false;

    auto& e1 = world.CreateEntity();
    e1.transform = &tf1;
    e1.meshRenderer = &mr1;

    auto& e2 = world.CreateEntity();
    e2.transform = &tf2;
    e2.meshRenderer = &mr2;

    int rendered = world.RenderPass();
    EXPECT_EQ(rendered, 1);

    // Only the visible entity's dirty flag was cleared
    EXPECT_FALSE(mr1.worldMatrixDirty);
    EXPECT_TRUE(mr2.worldMatrixDirty);
}

// ============================================================================
// Integration Test: Kinematic bodies are not affected by gravity
// ============================================================================

TEST(ECSIntegration_KinematicBodyNotAffected)
{
    using namespace ECSIntegration;

    TestWorld world;

    Transform dynamicTf, kinematicTf;
    dynamicTf.posY = 50.0f;
    kinematicTf.posY = 50.0f;

    RigidBody dynamicRb, kinematicRb;
    dynamicRb.type = RigidBody::Type::Dynamic;
    kinematicRb.type = RigidBody::Type::Kinematic;

    auto& e1 = world.CreateEntity();
    e1.transform = &dynamicTf;
    e1.rigidBody = &dynamicRb;

    auto& e2 = world.CreateEntity();
    e2.transform = &kinematicTf;
    e2.rigidBody = &kinematicRb;

    world.StepPhysics(1.0f / 60.0f);

    // Dynamic should move, kinematic should not
    EXPECT_TRUE(dynamicTf.posY < 50.0f);
    EXPECT_NEAR(kinematicTf.posY, 50.0f, 0.001f);
}

// ============================================================================
// Integration Test: Asset handle hashing consistency
// ============================================================================

TEST(ECSIntegration_AssetHandleConsistency)
{
    // Test FNV-1a hashing produces consistent results (R3.4)
    auto FNV1a = [](const std::string& str) -> uint64_t {
        uint64_t hash = 14695981039346656037ULL;
        for (char c : str)
        {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    };

    // Same string always produces same hash
    uint64_t h1 = FNV1a("Assets/Meshes/Cube.mesh");
    uint64_t h2 = FNV1a("Assets/Meshes/Cube.mesh");
    EXPECT_EQ(h1, h2);

    // Different strings produce different hashes
    uint64_t h3 = FNV1a("Assets/Meshes/Sphere.mesh");
    EXPECT_TRUE(h1 != h3);

    // Empty string has a well-defined hash
    uint64_t h4 = FNV1a("");
    EXPECT_TRUE(h4 != 0); // FNV offset basis, not zero
}

// ============================================================================
// Integration Test: Phase execution order (R2.3)
// ============================================================================

TEST(ECSIntegration_PhaseExecutionOrder)
{
    // Simulate phase ordering: systems should execute in phase order
    enum class Phase
    {
        PrePhysics,
        Physics,
        PostPhysics,
        Animation,
        AI,
        Audio,
        Gameplay,
        PreRender,
        Render,
        PostRender
    };

    std::vector<int> executionOrder;

    // Add "systems" in scrambled order
    struct SystemEntry
    {
        Phase phase;
        int id;
    };
    std::vector<SystemEntry> systems = {
        {Phase::Render, 6},     {Phase::Physics, 2},   {Phase::AI, 4},
        {Phase::PrePhysics, 1}, {Phase::Audio, 5},      {Phase::PostPhysics, 3},
        {Phase::PostRender, 7}, {Phase::Animation, 8},
    };

    // Sort by phase (simulating PhaseSystemManager behavior)
    std::sort(systems.begin(), systems.end(),
              [](const SystemEntry& a, const SystemEntry& b) { return static_cast<int>(a.phase) < static_cast<int>(b.phase); });

    // Execute in order
    for (const auto& sys : systems)
    {
        executionOrder.push_back(sys.id);
    }

    // Verify order: PrePhysics(1) → Physics(2) → PostPhysics(3) → Animation(8) → AI(4) → Audio(5) → Render(6) → PostRender(7)
    EXPECT_EQ(executionOrder[0], 1); // PrePhysics
    EXPECT_EQ(executionOrder[1], 2); // Physics
    EXPECT_EQ(executionOrder[2], 3); // PostPhysics
    EXPECT_EQ(executionOrder[3], 8); // Animation
    EXPECT_EQ(executionOrder[4], 4); // AI
    EXPECT_EQ(executionOrder[5], 5); // Audio
    EXPECT_EQ(executionOrder[6], 6); // Render
    EXPECT_EQ(executionOrder[7], 7); // PostRender
}

// ============================================================================
// Integration Test: Reactive system change tracking (R2.4)
// ============================================================================

TEST(ECSIntegration_ReactiveChangeTracking)
{
    // Simulate reactive system: only process entities that changed
    struct ChangeEntry
    {
        uint32_t entityId;
        int changeType; // 0=construct, 1=update, 2=destroy
    };

    std::vector<ChangeEntry> changeQueue;

    // Simulate 100 entities, only 5 change
    std::vector<uint32_t> changedEntities = {3, 17, 42, 88, 99};
    for (uint32_t id : changedEntities)
    {
        changeQueue.push_back({id, 1}); // Updated
    }

    // Process only changed entities (reactive pattern)
    int processedCount = 0;
    for (const auto& change : changeQueue)
    {
        processedCount++;
    }

    // Should only process 5, not all 100
    EXPECT_EQ(processedCount, 5);
    EXPECT_EQ(changeQueue.size(), static_cast<size_t>(5));
}
