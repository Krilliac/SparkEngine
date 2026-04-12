/**
 * @file TestSystemManagerIntegration.cpp
 * @brief Integration tests for ECS SystemManager orchestration
 *
 * Tests the SystemManager that owns and ticks all ECS systems.
 * Uses lightweight custom systems (no GPU, no physics) to verify
 * execution order, enable/disable, and system lookup.
 */

#include "TestFramework.h"
#include "Engine/ECS/Systems/ECSystems.h"
#include "Engine/ECS/Components.h"

using namespace Spark::ECS;

// ============================================================================
// Test Helpers — lightweight instrumented systems
// ============================================================================

namespace
{

    // Records the order it was updated in a shared counter
    class OrderTrackingSystem : public ISystem
    {
      public:
        OrderTrackingSystem(const char* name, std::vector<std::string>& log) : m_name(name), m_log(log) {}

        void Update(World& /*world*/, float /*dt*/) override { m_log.push_back(m_name); }

        const char* GetName() const override { return m_name; }

      private:
        const char* m_name;
        std::vector<std::string>& m_log;
    };

    // Counts how many times Update was called
    class CountingSystem : public ISystem
    {
      public:
        CountingSystem(const char* name) : m_name(name) {}

        void Update(World& /*world*/, float dt) override
        {
            m_updateCount++;
            m_totalDt += dt;
        }

        const char* GetName() const override { return m_name; }
        int GetUpdateCount() const { return m_updateCount; }
        float GetTotalDt() const { return m_totalDt; }

      private:
        const char* m_name;
        int m_updateCount = 0;
        float m_totalDt = 0.0f;
    };

    // A system that creates entities during Update
    class EntityCreatorSystem : public ISystem
    {
      public:
        void Update(World& world, float /*dt*/) override
        {
            auto entity = world.CreateEntity("TestEntity");
            m_createdEntities.push_back(entity);
        }

        const char* GetName() const override { return "EntityCreatorSystem"; }
        const std::vector<EntityID>& GetCreatedEntities() const { return m_createdEntities; }

      private:
        std::vector<EntityID> m_createdEntities;
    };

} // namespace

// ============================================================================
// Basic Lifecycle Tests
// ============================================================================

TEST(SystemManager_DefaultConstruction_HasNoSystems)
{
    SystemManager mgr;
    EXPECT_EQ(mgr.GetSystemCount(), 0u);
}

TEST(SystemManager_AddSystem_IncrementsCount)
{
    SystemManager mgr;
    std::vector<std::string> log;
    mgr.AddSystem<OrderTrackingSystem>("SystemA", log);
    EXPECT_EQ(mgr.GetSystemCount(), 1u);

    mgr.AddSystem<OrderTrackingSystem>("SystemB", log);
    EXPECT_EQ(mgr.GetSystemCount(), 2u);
}

TEST(SystemManager_AddSystem_ReturnsNonNullPointer)
{
    SystemManager mgr;
    std::vector<std::string> log;
    auto* sys = mgr.AddSystem<OrderTrackingSystem>("TestSys", log);
    EXPECT_TRUE(sys != nullptr);
}

// ============================================================================
// Execution Order Tests
// ============================================================================

TEST(SystemManager_UpdateAll_ExecutesInRegistrationOrder)
{
    SystemManager mgr;
    std::vector<std::string> log;
    World world;

    mgr.AddSystem<OrderTrackingSystem>("Physics", log);
    mgr.AddSystem<OrderTrackingSystem>("Animation", log);
    mgr.AddSystem<OrderTrackingSystem>("AI", log);
    mgr.AddSystem<OrderTrackingSystem>("Audio", log);
    mgr.AddSystem<OrderTrackingSystem>("Lifecycle", log);
    mgr.AddSystem<OrderTrackingSystem>("Render", log);

    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(log.size(), 6u);
    EXPECT_EQ(log[0], std::string("Physics"));
    EXPECT_EQ(log[1], std::string("Animation"));
    EXPECT_EQ(log[2], std::string("AI"));
    EXPECT_EQ(log[3], std::string("Audio"));
    EXPECT_EQ(log[4], std::string("Lifecycle"));
    EXPECT_EQ(log[5], std::string("Render"));
}

TEST(SystemManager_UpdateAll_MultipleFrames_AllSystemsTickEachFrame)
{
    SystemManager mgr;
    World world;

    auto* counter = mgr.AddSystem<CountingSystem>("Counter");

    for (int i = 0; i < 100; ++i)
        mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(counter->GetUpdateCount(), 100);
    EXPECT_NEAR(counter->GetTotalDt(), 1.6f, 0.01f);
}

// ============================================================================
// Enable/Disable Tests
// ============================================================================

TEST(SystemManager_DisabledSystem_SkippedDuringUpdate)
{
    SystemManager mgr;
    std::vector<std::string> log;
    World world;

    mgr.AddSystem<OrderTrackingSystem>("AlwaysRuns", log);
    auto* skipped = mgr.AddSystem<OrderTrackingSystem>("Disabled", log);
    mgr.AddSystem<OrderTrackingSystem>("AlsoRuns", log);

    skipped->SetEnabled(false);
    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], std::string("AlwaysRuns"));
    EXPECT_EQ(log[1], std::string("AlsoRuns"));
}

TEST(SystemManager_ReenableSystem_ResumesUpdating)
{
    SystemManager mgr;
    World world;

    auto* counter = mgr.AddSystem<CountingSystem>("Toggle");

    counter->SetEnabled(false);
    mgr.UpdateAll(world, 0.016f);
    EXPECT_EQ(counter->GetUpdateCount(), 0);

    counter->SetEnabled(true);
    mgr.UpdateAll(world, 0.016f);
    EXPECT_EQ(counter->GetUpdateCount(), 1);
}

// ============================================================================
// System Lookup Tests
// ============================================================================

TEST(SystemManager_GetSystem_FindsByName)
{
    SystemManager mgr;
    std::vector<std::string> log;

    auto* added = mgr.AddSystem<OrderTrackingSystem>("MySystem", log);
    auto* found = mgr.GetSystem("MySystem");

    EXPECT_TRUE(found != nullptr);
    EXPECT_TRUE(found == added);
}

TEST(SystemManager_GetSystem_ReturnsNullForUnknown)
{
    SystemManager mgr;
    auto* result = mgr.GetSystem("NonexistentSystem");
    EXPECT_TRUE(result == nullptr);
}

// ============================================================================
// World Interaction Tests
// ============================================================================

TEST(SystemManager_SystemCanCreateEntities)
{
    SystemManager mgr;
    World world;

    auto* creator = mgr.AddSystem<EntityCreatorSystem>();

    mgr.UpdateAll(world, 0.016f);
    mgr.UpdateAll(world, 0.016f);
    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(creator->GetCreatedEntities().size(), 3u);

    // Verify the entities have name components (proof they were created in the world)
    for (auto entity : creator->GetCreatedEntities())
    {
        EXPECT_TRUE(world.HasComponent<NameComponent>(entity));
    }
}

TEST(SystemManager_SystemCanReadComponents)
{
    SystemManager mgr;
    World world;

    // Create an entity with a Transform
    auto entity = world.CreateEntity("TestObj");
    auto& tf = world.AddComponent<Transform>(entity);
    tf.position = {1.0f, 2.0f, 3.0f};

    // A counting system can coexist with entities
    auto* counter = mgr.AddSystem<CountingSystem>("Reader");
    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(counter->GetUpdateCount(), 1);
    EXPECT_TRUE(world.HasComponent<Transform>(entity));
}
