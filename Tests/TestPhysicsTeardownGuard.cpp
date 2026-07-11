/**
 * @file TestPhysicsTeardownGuard.cpp
 * @brief Regression tests for the physics teardown-order guard (W10 exit AV).
 *
 * A module that fails OnLoad is only destroyed at ModuleManager::UnloadAll,
 * which historically ran AFTER ShutdownPhysics — its destructor then released
 * shared_ptr<PhysicsBody> handles through PhysicsSystem::RemoveBody into a
 * destroyed Jolt world and crashed the process at exit. Two fixes landed:
 *  1. ModuleManager::InitializeAll destroys a failed module's instance
 *     immediately (while physics is still alive).
 *  2. PhysicsSystem::RemoveBody is a guarded, WARN-logged no-op after
 *     Shutdown() — defense in depth for any remaining late releases.
 * This test locks in fix #2: Initialize → CreateBody → Shutdown → RemoveBody
 * must not crash.
 */

#include "TestFramework.h"
#include "Physics/PhysicsSystem.h"

#include <memory>

TEST(PhysicsTeardown_RemoveBodyAfterShutdown_IsSafeNoOp)
{
    // Heap-allocate and shut down inside the test body: PhysicsSystem logs via
    // SimpleConsole, so it must never outlive the singleton (see
    // TestEngineLoadTest.cpp for the same pattern).
    auto physics = std::make_unique<PhysicsSystem>();

    const HRESULT hr = physics->Initialize();
#ifdef SPARK_TEST_HAS_PHYSICS
    ASSERT_TRUE(SUCCEEDED(hr));
#else
    (void)hr; // stub backend: Initialize result is not the subject under test
#endif

    // Default desc: dynamic 2x2x2 box at origin, mass 1 — shape details are
    // irrelevant here; we only need a live wrapper whose handle outlives the world.
    PhysicsBodyDesc desc;
    desc.name = "teardown_guard_box";

    std::shared_ptr<PhysicsBody> body = physics->CreateBody(desc);
#ifdef SPARK_TEST_HAS_PHYSICS
    ASSERT_TRUE(body != nullptr);
    EXPECT_EQ(physics->GetBodies().size(), static_cast<size_t>(1));
#endif

    // Tear the world down while the shared_ptr handle is still held — exactly
    // what a failed-boot module's destructor used to do at engine exit.
    physics->Shutdown();
    EXPECT_TRUE(physics->GetBodies().empty());

    // W10 regression: every one of these must be a safe no-op, not an AV.
    physics->RemoveBody(body); // first release after teardown (logs one WARN)
    physics->RemoveBody(body); // repeat release (WARN suppressed, still safe)
    physics->RemoveBody(nullptr);
    physics->RemoveAllBodies();
    EXPECT_TRUE(physics->GetBodies().empty());

    // Double-Shutdown and destructor (which calls Shutdown again) must be safe.
    physics->Shutdown();
    physics.reset();

    // The wrapper handle itself must still be releasable after the system died
    // (~PhysicsBody does not touch Jolt; removal is PhysicsSystem's job).
    body.reset();
}
