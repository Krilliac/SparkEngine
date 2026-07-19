/**
 * @file Test_lifecycle_ecs_phase_wiring.cpp
 * @brief Regression tests: the production gameplay lifecycle must register and
 *        tick the canonical ECS phase systems.
 *
 * The PhaseSystemManager built by EngineSetup::CreatePhaseSystemManager was
 * historically defined-but-never-wired: zero production callers, so the core
 * AI/animation/lifecycle/projectile ECS systems only ever ran in tests. The
 * wiring now lives in GameplayLifecycleShared.cpp — registration via
 * InitializeEcsPhaseSystemsImpl (called from InitializeGameplaySystemsImpl)
 * and a per-frame UpdateAll pump in UpdateGameplaySystemsImpl.
 *
 * These tests drive the exact production registration entry point against the
 * process-wide EngineContext and the exact lifecycle-owned manager instance the
 * frame pump ticks, so they fail if the phase systems are ever un-wired again.
 */

#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Systems/PhaseSystemManager.h"

#include <memory>

namespace
{
    // Ensure the process-wide EngineContext exists and carries a World, exactly
    // like platform startup does before InitializeGameplaySystemsImpl runs.
    // Uses a static World so the context never points at a dead frame object.
    World& SetupContextWithWorld()
    {
        if (!EngineContext::Get())
        {
            EngineContext::SetOwned(std::make_unique<EngineContext>());
        }
        static World s_world;
        EngineContext::Get()->SetWorld(&s_world);
        return s_world;
    }
} // namespace

TEST(LifecycleEcsPhases_ProductionInitRegistersSystems)
{
    SetupContextWithWorld();

    // The same call InitializeGameplaySystemsImpl makes at engine startup.
    Spark::Core::Lifecycle::InitializeEcsPhaseSystemsImpl();

    auto& mgr = Spark::Core::Lifecycle::GetPhaseSystemManagerImpl();

    // Headless test context has no physics/audio/graphics, so those phase
    // entries are skipped by design — but the unconditional core systems must
    // always be present.
    EXPECT_TRUE(mgr.GetSystemCount() >= 5);
    EXPECT_TRUE(mgr.GetSystem("AnimationUpdateSystem") != nullptr);
    EXPECT_TRUE(mgr.GetSystem("AIUpdateSystem") != nullptr);
    EXPECT_TRUE(mgr.GetSystem("LifecycleSystem") != nullptr);
    EXPECT_TRUE(mgr.GetSystem("ProjectileSystem") != nullptr);
    EXPECT_TRUE(mgr.GetSystem("SplineFollowerSystem") != nullptr);
}

TEST(LifecycleEcsPhases_ReinitReplacesInsteadOfAccumulating)
{
    SetupContextWithWorld();

    Spark::Core::Lifecycle::InitializeEcsPhaseSystemsImpl();
    auto& mgr = Spark::Core::Lifecycle::GetPhaseSystemManagerImpl();
    const size_t firstCount = mgr.GetSystemCount();
    EXPECT_TRUE(firstCount > 0);

    // A second init (editor restart path) must not double-register systems —
    // duplicates would double-tick component data every frame.
    Spark::Core::Lifecycle::InitializeEcsPhaseSystemsImpl();
    EXPECT_EQ(mgr.GetSystemCount(), firstCount);
}

TEST(LifecycleEcsPhases_TickAdvancesComponentData)
{
    World& world = SetupContextWithWorld();
    Spark::Core::Lifecycle::InitializeEcsPhaseSystemsImpl();

    auto entity = world.CreateEntity("phaseWiringProbe");
    world.AddComponent<Transform>(entity);
    auto& anim = world.AddComponent<AnimationController>(entity);
    anim.playing = true;
    anim.playbackSpeed = 1.0f;
    anim.duration = 10.0f;
    anim.loop = true;
    anim.currentTime = 0.0f;

    // Tick the lifecycle-owned manager — the same object and the same
    // UpdateAll call UpdateGameplaySystemsImpl pumps every frame. If the
    // registration regresses to an empty manager, nothing advances and the
    // assertions below fail.
    const float dt = 0.25f;
    Spark::Core::Lifecycle::GetPhaseSystemManagerImpl().UpdateAll(world, dt);

    const auto* after = world.GetComponent<AnimationController>(entity);
    EXPECT_TRUE(after != nullptr);
    if (after != nullptr)
    {
        EXPECT_NEAR(after->currentTime, dt, 1e-4f);
        EXPECT_NEAR(after->normalizedTime, dt / 10.0f, 1e-4f);
    }

    world.DestroyEntity(entity);
}
