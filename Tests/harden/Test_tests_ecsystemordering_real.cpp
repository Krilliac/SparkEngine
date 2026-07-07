/**
 * @file Test_tests_ecsystemordering_real.cpp
 * @brief Real-class tests for Spark::ECS::PhaseSystemManager.
 *
 * TestECSystemOrdering.cpp validates a standalone COPY of the manager, so the
 * shipped PhaseSystemManager has no direct coverage. These tests drive the real
 * class: phase-order execution, per-phase insertion order, disabled-system
 * skipping, flat-list-after-phased ordering, and GetSystem hit/miss.
 */

#include "TestFramework.h"
#include "Engine/ECS/Systems/PhaseSystemManager.h"

#include <string>
#include <vector>

using Spark::ECS::ISystem;
using Spark::ECS::Phase;
using Spark::ECS::PhaseSystemManager;
// Note: World lives in the global namespace (a thin EnTT wrapper), so it is
// referenced unqualified below rather than via a Spark::ECS:: using.

namespace
{
    // A system that appends its name to a shared log each time it updates.
    class RecordingSystem : public ISystem
    {
      public:
        RecordingSystem(std::string name, std::vector<std::string>* log) : m_name(std::move(name)), m_log(log) {}

        void Update(World& /*world*/, float /*dt*/) override { m_log->push_back(m_name); }
        const char* GetName() const override { return m_name.c_str(); }

      private:
        std::string m_name;
        std::vector<std::string>* m_log;
    };
} // namespace

TEST(PhaseSystemManagerReal_ExecutesInPhaseOrder)
{
    std::vector<std::string> log;
    PhaseSystemManager mgr;
    World world;

    // Registered out of phase order — Render before Physics — but must run
    // Physics first because phases execute in Phase enum order.
    mgr.AddSystem<RecordingSystem>(Phase::Render, "RenderSys", &log);
    mgr.AddSystem<RecordingSystem>(Phase::Physics, "PhysicsSys", &log);

    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(log.size(), static_cast<size_t>(2));
    if (log.size() == 2)
    {
        EXPECT_EQ(log[0], std::string("PhysicsSys"));
        EXPECT_EQ(log[1], std::string("RenderSys"));
    }
}

TEST(PhaseSystemManagerReal_InsertionOrderWithinPhase)
{
    std::vector<std::string> log;
    PhaseSystemManager mgr;
    World world;

    mgr.AddSystem<RecordingSystem>(Phase::Gameplay, "G1", &log);
    mgr.AddSystem<RecordingSystem>(Phase::Gameplay, "G2", &log);
    mgr.AddSystem<RecordingSystem>(Phase::Gameplay, "G3", &log);

    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(log.size(), static_cast<size_t>(3));
    if (log.size() == 3)
    {
        EXPECT_EQ(log[0], std::string("G1"));
        EXPECT_EQ(log[1], std::string("G2"));
        EXPECT_EQ(log[2], std::string("G3"));
    }
}

TEST(PhaseSystemManagerReal_DisabledSystemSkipped)
{
    std::vector<std::string> log;
    PhaseSystemManager mgr;
    World world;

    auto* enabled = mgr.AddSystem<RecordingSystem>(Phase::AI, "Enabled", &log);
    auto* disabled = mgr.AddSystem<RecordingSystem>(Phase::AI, "Disabled", &log);
    (void)enabled;
    disabled->SetEnabled(false);

    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(log.size(), static_cast<size_t>(1));
    if (log.size() == 1)
        EXPECT_EQ(log[0], std::string("Enabled"));
}

TEST(PhaseSystemManagerReal_FlatSystemsRunAfterPhased)
{
    std::vector<std::string> log;
    PhaseSystemManager mgr;
    World world;

    // Flat (unphased) systems run after every phased system.
    mgr.AddSystem<RecordingSystem>("FlatSys", &log);
    mgr.AddSystem<RecordingSystem>(Phase::PreRender, "PhasedSys", &log);

    mgr.UpdateAll(world, 0.016f);

    EXPECT_EQ(log.size(), static_cast<size_t>(2));
    if (log.size() == 2)
    {
        EXPECT_EQ(log[0], std::string("PhasedSys"));
        EXPECT_EQ(log[1], std::string("FlatSys"));
    }
}

TEST(PhaseSystemManagerReal_GetSystemHitAndMiss)
{
    std::vector<std::string> log;
    PhaseSystemManager mgr;

    mgr.AddSystem<RecordingSystem>(Phase::Audio, "Findable", &log);
    EXPECT_EQ(mgr.GetSystemCount(), static_cast<size_t>(1));

    ISystem* hit = mgr.GetSystem("Findable");
    EXPECT_TRUE(hit != nullptr);
    if (hit)
        EXPECT_EQ(std::string(hit->GetName()), std::string("Findable"));

    EXPECT_TRUE(mgr.GetSystem("Missing") == nullptr);
}
