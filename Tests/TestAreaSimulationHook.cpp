/**
 * @file TestAreaSimulationHook.cpp
 * @brief Tests for the AreaServer IAreaSimulation plug-in hook
 *
 * Verifies that a game-supplied IAreaSimulation attached via
 * AreaServer::SetSimulation() is ticked once per AreaServer::Tick() with the
 * fixed delta, that a null simulation is safe, and that detaching stops the
 * callbacks. Uses externally-driven Tick() only — no Start(), no tick thread,
 * no sockets — so it is CI-safe.
 */

#include "TestFramework.h"

#ifdef ENABLE_NETWORKING

#include "Engine/Networking/AreaServer.h"
#include "Engine/Networking/IAreaSimulation.h"

#include <memory>

using Spark::Net::AreaServer;
using Spark::Net::AreaServerConfig;
using Spark::Net::IAreaSimulation;

namespace
{
    class CountingSimulation : public IAreaSimulation
    {
      public:
        void OnAreaTick(float fixedDt) override
        {
            ++tickCount;
            lastDt = fixedDt;
        }

        int tickCount = 0;
        float lastDt = -1.0f;
    };
} // namespace

TEST(AreaSimHook_TickWithoutSimulationIsSafe)
{
    AreaServer area;
    // No simulation attached — must not crash.
    area.Tick(1.0f / 60.0f);
    area.Tick(1.0f / 60.0f);
    EXPECT_FALSE(area.IsRunning()); // externally driven, never Start()ed
}

TEST(AreaSimHook_SimulationTickedOncePerTick)
{
    AreaServer area;
    CountingSimulation sim;
    area.SetSimulation(&sim);

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 5; ++i)
        area.Tick(dt);

    EXPECT_EQ(sim.tickCount, 5);
    EXPECT_NEAR(sim.lastDt, dt, 1e-6f);
}

TEST(AreaSimHook_DetachStopsCallbacks)
{
    AreaServer area;
    CountingSimulation sim;

    area.SetSimulation(&sim);
    area.Tick(1.0f / 60.0f);
    EXPECT_EQ(sim.tickCount, 1);

    area.SetSimulation(nullptr);
    area.Tick(1.0f / 60.0f);
    area.Tick(1.0f / 60.0f);
    EXPECT_EQ(sim.tickCount, 1); // unchanged after detach
}

TEST(AreaSimHook_ReattachDifferentSimulation)
{
    AreaServer area;
    CountingSimulation simA;
    CountingSimulation simB;

    area.SetSimulation(&simA);
    area.Tick(1.0f / 60.0f);

    area.SetSimulation(&simB);
    area.Tick(1.0f / 60.0f);
    area.Tick(1.0f / 60.0f);

    EXPECT_EQ(simA.tickCount, 1);
    EXPECT_EQ(simB.tickCount, 2);
}

TEST(AreaSimHook_InterfaceContract)
{
    // IAreaSimulation must be destructible through the base pointer
    // (virtual destructor) — the engine holds it as IAreaSimulation*.
    std::unique_ptr<IAreaSimulation> sim = std::make_unique<CountingSimulation>();
    sim->OnAreaTick(0.016f);
    auto* counting = static_cast<CountingSimulation*>(sim.get());
    EXPECT_EQ(counting->tickCount, 1);
    EXPECT_NEAR(counting->lastDt, 0.016f, 1e-6f);
    sim.reset(); // must not leak/crash
    EXPECT_TRUE(true);
}

TEST(AreaSimHook_TickRateDeltaMatchesConfig)
{
    // The AreaServer tick loop passes fixedDt = 1 / tickRate. Externally we
    // drive the same contract: whatever dt Tick() receives reaches the sim.
    AreaServer area;
    CountingSimulation sim;
    area.SetSimulation(&sim);

    AreaServerConfig cfg; // defaults: tickRate = 60
    const float fixedDt = 1.0f / cfg.tickRate;
    area.Tick(fixedDt);
    EXPECT_NEAR(sim.lastDt, fixedDt, 1e-6f);
}

#endif // ENABLE_NETWORKING
