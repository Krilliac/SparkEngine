/**
 * @file TestECSystemOrdering.cpp
 * @brief Tests for ECS SystemManager: system registration, execution ordering,
 *        entity query filtering, lifecycle callbacks, and system orchestration.
 *
 * Standalone reimplementation — mirrors Spark::ECS::SystemManager and ISystem
 * without engine dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    // --- Minimal World stub ---

    using EntityID = uint32_t;

    struct World
    {
        struct Entity
        {
            EntityID id = 0;
            bool active = true;
            float health = 100.0f;
            float x = 0.0f, y = 0.0f, z = 0.0f;
        };

        std::vector<Entity> entities;

        EntityID CreateEntity()
        {
            EntityID id = static_cast<EntityID>(entities.size()) + 1;
            entities.push_back({id, true, 100.0f, 0.0f, 0.0f, 0.0f});
            return id;
        }

        Entity* GetEntity(EntityID id)
        {
            for (auto& e : entities)
            {
                if (e.id == id)
                    return &e;
            }
            return nullptr;
        }

        size_t GetActiveEntityCount() const
        {
            size_t count = 0;
            for (auto& e : entities)
            {
                if (e.active)
                    count++;
            }
            return count;
        }
    };

    // --- ISystem interface (mirrors Spark::ECS::ISystem) ---

    class ISystem
    {
      public:
        virtual ~ISystem() = default;
        virtual void Initialize(World& world) { (void)world; }
        virtual void Update(World& world, float deltaTime) = 0;
        virtual void Shutdown(World& world) { (void)world; }
        virtual std::string_view GetName() const = 0;
        virtual int GetPriority() const { return 0; }
    };

    // --- Concrete test systems that record execution order ---

    static std::vector<std::string> g_executionOrder;

    class PhysicsUpdateSystem : public ISystem
    {
      public:
        void Update(World& world, float deltaTime) override
        {
            g_executionOrder.push_back("Physics");
            for (auto& e : world.entities)
            {
                if (e.active)
                    e.y += -9.81f * deltaTime; // gravity
            }
            m_updateCount++;
        }
        std::string_view GetName() const override { return "PhysicsUpdateSystem"; }
        int GetPriority() const override { return 0; } // highest priority
        int GetUpdateCount() const { return m_updateCount; }

      private:
        int m_updateCount = 0;
    };

    class AnimationUpdateSystem : public ISystem
    {
      public:
        void Update(World& /*world*/, float /*deltaTime*/) override
        {
            g_executionOrder.push_back("Animation");
            m_updateCount++;
        }
        std::string_view GetName() const override { return "AnimationUpdateSystem"; }
        int GetPriority() const override { return 1; }
        int GetUpdateCount() const { return m_updateCount; }

      private:
        int m_updateCount = 0;
    };

    class AIUpdateSystem : public ISystem
    {
      public:
        void Update(World& /*world*/, float /*deltaTime*/) override
        {
            g_executionOrder.push_back("AI");
            m_updateCount++;
        }
        std::string_view GetName() const override { return "AIUpdateSystem"; }
        int GetPriority() const override { return 2; }
        int GetUpdateCount() const { return m_updateCount; }

      private:
        int m_updateCount = 0;
    };

    class AudioUpdateSystem : public ISystem
    {
      public:
        void Update(World& /*world*/, float /*deltaTime*/) override
        {
            g_executionOrder.push_back("Audio");
            m_updateCount++;
        }
        std::string_view GetName() const override { return "AudioUpdateSystem"; }
        int GetPriority() const override { return 3; }
        int GetUpdateCount() const { return m_updateCount; }

      private:
        int m_updateCount = 0;
    };

    class LifecycleSystem : public ISystem
    {
      public:
        using DeathCallback = std::function<void(EntityID)>;

        void Update(World& world, float /*deltaTime*/) override
        {
            g_executionOrder.push_back("Lifecycle");
            for (auto& e : world.entities)
            {
                if (e.active && e.health <= 0.0f)
                {
                    e.active = false;
                    if (m_deathCallback)
                        m_deathCallback(e.id);
                }
            }
        }
        std::string_view GetName() const override { return "LifecycleSystem"; }
        int GetPriority() const override { return 4; }

        void SetDeathCallback(DeathCallback cb) { m_deathCallback = std::move(cb); }

      private:
        DeathCallback m_deathCallback;
    };

    class RenderSystem : public ISystem
    {
      public:
        void Update(World& world, float /*deltaTime*/) override
        {
            g_executionOrder.push_back("Render");
            m_renderedCount = 0;
            for (auto& e : world.entities)
            {
                if (e.active)
                    m_renderedCount++;
            }
        }
        std::string_view GetName() const override { return "RenderSystem"; }
        int GetPriority() const override { return 5; } // lowest priority (last)
        int GetRenderedEntityCount() const { return m_renderedCount; }

      private:
        int m_renderedCount = 0;
    };

    // --- SystemManager (mirrors Spark::ECS::SystemManager) ---

    class SystemManager
    {
      public:
        template <typename T, typename... Args> T* AddSystem(Args&&... args)
        {
            auto system = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = system.get();
            m_systems.push_back(std::move(system));
            // Keep sorted by priority
            std::sort(m_systems.begin(), m_systems.end(),
                      [](const auto& a, const auto& b) { return a->GetPriority() < b->GetPriority(); });
            return ptr;
        }

        void InitializeAll(World& world)
        {
            for (auto& sys : m_systems)
                sys->Initialize(world);
            m_initialized = true;
        }

        void UpdateAll(World& world, float deltaTime)
        {
            for (auto& sys : m_systems)
                sys->Update(world, deltaTime);
        }

        void ShutdownAll(World& world)
        {
            // Shutdown in reverse order
            for (auto it = m_systems.rbegin(); it != m_systems.rend(); ++it)
                (*it)->Shutdown(world);
            m_initialized = false;
        }

        ISystem* GetSystem(std::string_view name)
        {
            for (auto& sys : m_systems)
            {
                if (sys->GetName() == name)
                    return sys.get();
            }
            return nullptr;
        }

        size_t GetSystemCount() const { return m_systems.size(); }
        bool IsInitialized() const { return m_initialized; }

      private:
        std::vector<std::unique_ptr<ISystem>> m_systems;
        bool m_initialized = false;
    };

} // anonymous namespace

// ============================================================================
// System Registration Tests
// ============================================================================

TEST(ECSystem_AddSingleSystem)
{
    SystemManager mgr;
    auto* physics = mgr.AddSystem<PhysicsUpdateSystem>();
    EXPECT_TRUE(physics != nullptr);
    EXPECT_EQ(mgr.GetSystemCount(), (size_t)1);
}

TEST(ECSystem_AddMultipleSystems)
{
    SystemManager mgr;
    mgr.AddSystem<PhysicsUpdateSystem>();
    mgr.AddSystem<AnimationUpdateSystem>();
    mgr.AddSystem<AIUpdateSystem>();
    mgr.AddSystem<AudioUpdateSystem>();
    mgr.AddSystem<LifecycleSystem>();
    mgr.AddSystem<RenderSystem>();
    EXPECT_EQ(mgr.GetSystemCount(), (size_t)6);
}

TEST(ECSystem_GetSystemByName)
{
    SystemManager mgr;
    mgr.AddSystem<PhysicsUpdateSystem>();
    mgr.AddSystem<AIUpdateSystem>();

    auto* ai = mgr.GetSystem("AIUpdateSystem");
    EXPECT_TRUE(ai != nullptr);
    EXPECT_EQ(ai->GetName(), std::string_view("AIUpdateSystem"));

    auto* missing = mgr.GetSystem("NonExistentSystem");
    EXPECT_TRUE(missing == nullptr);
}

// ============================================================================
// Execution Order Tests — THE CRITICAL PATH
// ============================================================================

TEST(ECSystem_ExecutionOrder_PhysicsFirst)
{
    g_executionOrder.clear();
    SystemManager mgr;
    World world;

    // Add in WRONG order — SystemManager should sort by priority
    mgr.AddSystem<RenderSystem>();
    mgr.AddSystem<AIUpdateSystem>();
    mgr.AddSystem<PhysicsUpdateSystem>();
    mgr.AddSystem<AudioUpdateSystem>();
    mgr.AddSystem<AnimationUpdateSystem>();
    mgr.AddSystem<LifecycleSystem>();

    world.CreateEntity();
    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(g_executionOrder.size(), (size_t)6);
    EXPECT_EQ(g_executionOrder[0], std::string("Physics"));
    EXPECT_EQ(g_executionOrder[1], std::string("Animation"));
    EXPECT_EQ(g_executionOrder[2], std::string("AI"));
    EXPECT_EQ(g_executionOrder[3], std::string("Audio"));
    EXPECT_EQ(g_executionOrder[4], std::string("Lifecycle"));
    EXPECT_EQ(g_executionOrder[5], std::string("Render"));
}

TEST(ECSystem_ExecutionOrder_ConsistentAcrossFrames)
{
    g_executionOrder.clear();
    SystemManager mgr;
    World world;

    mgr.AddSystem<PhysicsUpdateSystem>();
    mgr.AddSystem<AnimationUpdateSystem>();
    mgr.AddSystem<RenderSystem>();

    world.CreateEntity();

    mgr.UpdateAll(world, 0.016f);
    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(g_executionOrder.size(), (size_t)6);
    // Frame 1
    EXPECT_EQ(g_executionOrder[0], std::string("Physics"));
    EXPECT_EQ(g_executionOrder[1], std::string("Animation"));
    EXPECT_EQ(g_executionOrder[2], std::string("Render"));
    // Frame 2 — same order
    EXPECT_EQ(g_executionOrder[3], std::string("Physics"));
    EXPECT_EQ(g_executionOrder[4], std::string("Animation"));
    EXPECT_EQ(g_executionOrder[5], std::string("Render"));
}

// ============================================================================
// Lifecycle Tests
// ============================================================================

TEST(ECSystem_InitializeAll)
{
    SystemManager mgr;
    World world;

    mgr.AddSystem<PhysicsUpdateSystem>();
    mgr.AddSystem<RenderSystem>();

    EXPECT_FALSE(mgr.IsInitialized());
    mgr.InitializeAll(world);
    EXPECT_TRUE(mgr.IsInitialized());
}

TEST(ECSystem_ShutdownAll)
{
    SystemManager mgr;
    World world;

    mgr.AddSystem<PhysicsUpdateSystem>();
    mgr.InitializeAll(world);
    EXPECT_TRUE(mgr.IsInitialized());

    mgr.ShutdownAll(world);
    EXPECT_FALSE(mgr.IsInitialized());
}

// ============================================================================
// System Update Behavior Tests
// ============================================================================

TEST(ECSystem_PhysicsAppliesGravity)
{
    g_executionOrder.clear();
    SystemManager mgr;
    World world;

    mgr.AddSystem<PhysicsUpdateSystem>();
    auto id = world.CreateEntity();

    mgr.UpdateAll(world, 1.0f);

    auto* entity = world.GetEntity(id);
    EXPECT_TRUE(entity != nullptr);
    EXPECT_NEAR(entity->y, -9.81f, 0.01f);
}

TEST(ECSystem_LifecycleDeathCallback)
{
    g_executionOrder.clear();
    SystemManager mgr;
    World world;

    auto* lifecycle = mgr.AddSystem<LifecycleSystem>();

    EntityID deadEntity = 0;
    lifecycle->SetDeathCallback([&](EntityID id) { deadEntity = id; });

    auto id = world.CreateEntity();
    world.GetEntity(id)->health = -1.0f;

    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(deadEntity, id);
    EXPECT_FALSE(world.GetEntity(id)->active);
}

TEST(ECSystem_RenderCountsActiveEntities)
{
    g_executionOrder.clear();
    SystemManager mgr;
    World world;

    auto* render = mgr.AddSystem<RenderSystem>();
    world.CreateEntity();
    world.CreateEntity();
    auto id3 = world.CreateEntity();
    world.GetEntity(id3)->active = false;

    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(render->GetRenderedEntityCount(), 2);
}

TEST(ECSystem_PhysicsUpdateCount)
{
    g_executionOrder.clear();
    SystemManager mgr;
    World world;

    auto* physics = mgr.AddSystem<PhysicsUpdateSystem>();
    world.CreateEntity();

    mgr.UpdateAll(world, 0.016f);
    mgr.UpdateAll(world, 0.016f);
    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(physics->GetUpdateCount(), 3);
}

// ============================================================================
// Entity Query Filtering
// ============================================================================

TEST(ECSystem_ActiveEntityCount)
{
    World world;
    world.CreateEntity();
    world.CreateEntity();
    auto id = world.CreateEntity();
    world.GetEntity(id)->active = false;

    EXPECT_EQ(world.GetActiveEntityCount(), (size_t)2);
}

TEST(ECSystem_EmptyWorldUpdate)
{
    g_executionOrder.clear();
    SystemManager mgr;
    World world;

    mgr.AddSystem<PhysicsUpdateSystem>();
    mgr.AddSystem<RenderSystem>();

    // No entities — should not crash
    EXPECT_NO_THROW(mgr.UpdateAll(world, 0.016f));
}

TEST(ECSystem_ZeroSystemsUpdate)
{
    SystemManager mgr;
    World world;
    world.CreateEntity();

    // No systems — should not crash
    EXPECT_NO_THROW(mgr.UpdateAll(world, 0.016f));
    EXPECT_EQ(mgr.GetSystemCount(), (size_t)0);
}

// ============================================================================
// Integration: Full Pipeline Order
// ============================================================================

TEST(ECSystem_FullPipeline_LifecycleBeforeRender)
{
    g_executionOrder.clear();
    SystemManager mgr;
    World world;

    auto* lifecycle = mgr.AddSystem<LifecycleSystem>();
    auto* render = mgr.AddSystem<RenderSystem>();
    mgr.AddSystem<PhysicsUpdateSystem>();

    // Entity killed before render — render should see fewer entities
    auto id1 = world.CreateEntity();
    auto id2 = world.CreateEntity();
    world.GetEntity(id1)->health = -1.0f;

    lifecycle->SetDeathCallback([](EntityID) {});

    mgr.UpdateAll(world, 0.016f);

    // Lifecycle (priority 4) runs before Render (priority 5)
    // So entity should be deactivated before render counts it
    EXPECT_EQ(render->GetRenderedEntityCount(), 1);
    (void)id2;
}
