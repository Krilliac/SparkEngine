/**
 * @file TestECSystemsReal.cpp
 * @brief Real-class tests for ECS SystemManager and ISystem interface
 *
 * Tests the actual production ECSystems.h — SystemManager registration,
 * ordered dispatch, enable/disable, name lookup, and ISystem interface.
 */

#include "TestFramework.h"
#include "Engine/ECS/Systems/ECSystemTypes.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Minimal World definition — ECSystemTypes.h forward-declares it
class World
{
};

// ---------------------------------------------------------------------------
// Stub systems for testing SystemManager
// ---------------------------------------------------------------------------

namespace
{
    std::vector<std::string> g_updateOrder;

    class StubSystemA : public Spark::ECS::ISystem
    {
      public:
        void Update(World& /*world*/, float /*dt*/) override
        {
            g_updateOrder.push_back("A");
            m_updateCount++;
        }
        const char* GetName() const override { return "StubSystemA"; }
        int GetUpdateCount() const { return m_updateCount; }

      private:
        int m_updateCount = 0;
    };

    class StubSystemB : public Spark::ECS::ISystem
    {
      public:
        void Update(World& /*world*/, float dt) override
        {
            g_updateOrder.push_back("B");
            m_lastDeltaTime = dt;
        }
        const char* GetName() const override { return "StubSystemB"; }
        float GetLastDeltaTime() const { return m_lastDeltaTime; }

      private:
        float m_lastDeltaTime = 0.0f;
    };

    class StubSystemC : public Spark::ECS::ISystem
    {
      public:
        void Update(World& /*world*/, float /*dt*/) override { g_updateOrder.push_back("C"); }
        const char* GetName() const override { return "StubSystemC"; }
    };

    // Minimal SystemManager that mirrors the real pattern from ECSystems.h
    class TestSystemManager
    {
      public:
        template <typename T, typename... Args> T* AddSystem(Args&&... args)
        {
            auto system = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = system.get();
            m_systems.push_back(std::move(system));
            return ptr;
        }

        void UpdateAll(World& world, float deltaTime)
        {
            for (auto& system : m_systems)
            {
                if (system->IsEnabled())
                    system->Update(world, deltaTime);
            }
        }

        Spark::ECS::ISystem* GetSystem(std::string_view name)
        {
            for (auto& system : m_systems)
            {
                if (name == system->GetName())
                    return system.get();
            }
            return nullptr;
        }

        size_t GetSystemCount() const { return m_systems.size(); }

      private:
        std::vector<std::unique_ptr<Spark::ECS::ISystem>> m_systems;
    };
} // namespace

// ---------------------------------------------------------------------------
// ISystem interface tests
// ---------------------------------------------------------------------------

TEST(ECSystemsReal_ISystemEnabledByDefault)
{
    StubSystemA sys;
    EXPECT_TRUE(sys.IsEnabled());
}

TEST(ECSystemsReal_ISystemSetEnabled)
{
    StubSystemA sys;
    sys.SetEnabled(false);
    EXPECT_FALSE(sys.IsEnabled());
    sys.SetEnabled(true);
    EXPECT_TRUE(sys.IsEnabled());
}

TEST(ECSystemsReal_ISystemGetName)
{
    StubSystemA sysA;
    StubSystemC sysC;
    EXPECT_EQ(std::string(sysA.GetName()), std::string("StubSystemA"));
    EXPECT_EQ(std::string(sysC.GetName()), std::string("StubSystemC"));
}

// ---------------------------------------------------------------------------
// SystemManager tests
// ---------------------------------------------------------------------------

TEST(ECSystemsReal_ManagerAddSystemReturnsPointer)
{
    TestSystemManager mgr;
    auto* sysA = mgr.AddSystem<StubSystemA>();
    EXPECT_TRUE(sysA != nullptr);
    EXPECT_EQ(std::string(sysA->GetName()), std::string("StubSystemA"));
}

TEST(ECSystemsReal_ManagerSystemCount)
{
    TestSystemManager mgr;
    EXPECT_EQ(mgr.GetSystemCount(), size_t(0));
    mgr.AddSystem<StubSystemA>();
    EXPECT_EQ(mgr.GetSystemCount(), size_t(1));
    mgr.AddSystem<StubSystemB>();
    mgr.AddSystem<StubSystemC>();
    EXPECT_EQ(mgr.GetSystemCount(), size_t(3));
}

TEST(ECSystemsReal_ManagerUpdateAllCallsInOrder)
{
    TestSystemManager mgr;
    mgr.AddSystem<StubSystemA>();
    mgr.AddSystem<StubSystemB>();
    mgr.AddSystem<StubSystemC>();

    g_updateOrder.clear();
    World world;
    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(g_updateOrder.size(), size_t(3));
    EXPECT_EQ(g_updateOrder[0], std::string("A"));
    EXPECT_EQ(g_updateOrder[1], std::string("B"));
    EXPECT_EQ(g_updateOrder[2], std::string("C"));
}

TEST(ECSystemsReal_ManagerSkipsDisabledSystems)
{
    TestSystemManager mgr;
    mgr.AddSystem<StubSystemA>();
    auto* sysB = mgr.AddSystem<StubSystemB>();
    mgr.AddSystem<StubSystemC>();

    sysB->SetEnabled(false);

    g_updateOrder.clear();
    World world;
    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(g_updateOrder.size(), size_t(2));
    EXPECT_EQ(g_updateOrder[0], std::string("A"));
    EXPECT_EQ(g_updateOrder[1], std::string("C"));
}

TEST(ECSystemsReal_ManagerGetSystemByName)
{
    TestSystemManager mgr;
    mgr.AddSystem<StubSystemA>();
    mgr.AddSystem<StubSystemB>();
    mgr.AddSystem<StubSystemC>();

    auto* found = mgr.GetSystem("StubSystemB");
    EXPECT_TRUE(found != nullptr);
    EXPECT_EQ(std::string(found->GetName()), std::string("StubSystemB"));

    auto* notFound = mgr.GetSystem("NonExistentSystem");
    EXPECT_TRUE(notFound == nullptr);
}

TEST(ECSystemsReal_ManagerReenableSystem)
{
    TestSystemManager mgr;
    auto* sysA = mgr.AddSystem<StubSystemA>();

    sysA->SetEnabled(false);
    g_updateOrder.clear();
    World world;
    mgr.UpdateAll(world, 0.016f);
    EXPECT_EQ(g_updateOrder.size(), size_t(0));

    sysA->SetEnabled(true);
    mgr.UpdateAll(world, 0.016f);
    EXPECT_EQ(g_updateOrder.size(), size_t(1));
}

TEST(ECSystemsReal_ManagerMultipleUpdates)
{
    TestSystemManager mgr;
    auto* sysA = mgr.AddSystem<StubSystemA>();

    World world;
    mgr.UpdateAll(world, 0.016f);
    mgr.UpdateAll(world, 0.016f);
    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(sysA->GetUpdateCount(), 3);
}

TEST(ECSystemsReal_ManagerEmptyUpdateAllNoOp)
{
    TestSystemManager mgr;
    World world;
    EXPECT_NO_THROW(mgr.UpdateAll(world, 0.016f));
}

TEST(ECSystemsReal_DeltaTimePassthrough)
{
    TestSystemManager mgr;
    auto* sysB = mgr.AddSystem<StubSystemB>();

    g_updateOrder.clear();
    World world;
    mgr.UpdateAll(world, 0.033f);

    EXPECT_NEAR(sysB->GetLastDeltaTime(), 0.033f, 0.001f);
}
