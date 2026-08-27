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

TEST(PhysicsConstraints_RejectInvalidWorldAndBodyInputs)
{
    auto physics = std::make_unique<PhysicsSystem>();
    const XMFLOAT3 zero{0.0f, 0.0f, 0.0f};
    const XMFLOAT3 axis{0.0f, 1.0f, 0.0f};
    const XMMATRIX identity = XMMatrixIdentity();

    // Every production constraint entry point must reject use before Jolt is
    // initialized. This exercises the world and null-handle guards without
    // constructing test doubles for Jolt-owned state.
    EXPECT_TRUE(physics->CreateHingeConstraint(nullptr, nullptr, zero, zero, axis, axis) == nullptr);
    EXPECT_TRUE(physics->CreateSliderConstraint(nullptr, nullptr, identity, identity) == nullptr);
    EXPECT_TRUE(physics->CreateFixedConstraint(nullptr, nullptr, identity, identity) == nullptr);
    EXPECT_TRUE(physics->CreatePoint2PointConstraint(nullptr, nullptr, zero, zero) == nullptr);
    EXPECT_TRUE(physics->CreateConeTwistConstraint(nullptr, nullptr, identity, identity) == nullptr);
    EXPECT_TRUE(physics->CreateDistanceConstraint(nullptr, nullptr, zero, zero) == nullptr);
    EXPECT_TRUE(physics->CreateConeConstraint(nullptr, nullptr, zero, axis) == nullptr);
    EXPECT_TRUE(physics->CreateSixDOFConstraint(nullptr, nullptr, identity, identity) == nullptr);
    EXPECT_TRUE(physics->CreatePulleyConstraint(nullptr, nullptr, zero, zero, zero, zero) == nullptr);
    EXPECT_TRUE(physics->CreateGearConstraint(nullptr, nullptr, nullptr, nullptr) == nullptr);
    EXPECT_TRUE(physics->CreateRackAndPinionConstraint(nullptr, nullptr, nullptr, nullptr) == nullptr);
    EXPECT_TRUE(physics->CreatePathConstraint(nullptr, {zero, axis}, {axis, axis}) == nullptr);
    physics->RemoveConstraint(nullptr);

    const HRESULT hr = physics->Initialize();
#ifdef SPARK_TEST_HAS_PHYSICS
    ASSERT_TRUE(SUCCEEDED(hr));

    PhysicsBodyDesc desc;
    desc.name = "constraint_removed_body";
    auto removedBody = physics->CreateBody(desc);
    ASSERT_TRUE(removedBody != nullptr);
    physics->RemoveBody(removedBody);

    // A live wrapper is not sufficient: the underlying BodyID must still be
    // present in Jolt's world.
    EXPECT_TRUE(physics->CreateHingeConstraint(removedBody, removedBody, zero, zero, axis, axis) == nullptr);
    EXPECT_TRUE(physics->CreatePoint2PointConstraint(removedBody, nullptr, zero, zero) == nullptr);
    EXPECT_TRUE(physics->CreatePathConstraint(removedBody, {zero, axis}, {axis, axis}) == nullptr);
#else
    (void)hr;
#endif

    physics->Shutdown();
}

TEST(PhysicsConstraints_CreateAndRemoveRealJoltConstraints)
{
    auto physics = std::make_unique<PhysicsSystem>();
    const HRESULT hr = physics->Initialize();
#ifdef SPARK_TEST_HAS_PHYSICS
    ASSERT_TRUE(SUCCEEDED(hr));

    PhysicsBodyDesc firstDesc;
    firstDesc.name = "constraint_body_a";
    firstDesc.position = {-2.0f, 2.0f, 0.0f};
    PhysicsBodyDesc secondDesc;
    secondDesc.name = "constraint_body_b";
    secondDesc.position = {2.0f, 2.0f, 0.0f};
    auto first = physics->CreateBody(firstDesc);
    auto second = physics->CreateBody(secondDesc);
    ASSERT_TRUE(first != nullptr);
    ASSERT_TRUE(second != nullptr);

    const XMFLOAT3 origin{0.0f, 2.0f, 0.0f};
    const XMFLOAT3 up{0.0f, 1.0f, 0.0f};
    const XMMATRIX identity = XMMatrixIdentity();

    auto verifyAndRemove = [&](const std::shared_ptr<PhysicsConstraint>& constraint, ConstraintType expected)
    {
        ASSERT_TRUE(constraint != nullptr);
        EXPECT_TRUE(constraint->GetType() == expected);
        EXPECT_TRUE(constraint->GetJoltConstraint() != nullptr);
        physics->RemoveConstraint(constraint);
    };

    verifyAndRemove(physics->CreateHingeConstraint(first, second, origin, origin, up, up), ConstraintType::Hinge);
    verifyAndRemove(physics->CreateSliderConstraint(first, second, identity, identity), ConstraintType::Slider);
    verifyAndRemove(physics->CreateFixedConstraint(first, second, identity, identity), ConstraintType::Fixed);
    verifyAndRemove(physics->CreatePoint2PointConstraint(first, second, origin, origin), ConstraintType::Point2Point);
    verifyAndRemove(physics->CreatePoint2PointConstraint(first, nullptr, origin, origin), ConstraintType::Point2Point);
    verifyAndRemove(physics->CreateConeTwistConstraint(first, second, identity, identity), ConstraintType::ConeTwist);
    verifyAndRemove(
        physics->CreateDistanceConstraint(first, second, firstDesc.position, secondDesc.position, 1.0f, 8.0f),
        ConstraintType::Distance);
    verifyAndRemove(physics->CreateConeConstraint(first, second, origin, up, 0.5f), ConstraintType::Cone);
    verifyAndRemove(physics->CreateSixDOFConstraint(first, second, identity, identity), ConstraintType::Generic6DOF);
    verifyAndRemove(physics->CreatePulleyConstraint(first, second, {-2.0f, 6.0f, 0.0f}, {2.0f, 6.0f, 0.0f},
                                                    firstDesc.position, secondDesc.position, 1.0f),
                    ConstraintType::Pulley);
    verifyAndRemove(physics->CreatePathConstraint(first, {{-2.0f, 2.0f, 0.0f}, {2.0f, 2.0f, 0.0f}},
                                                  {{1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}),
                    ConstraintType::Path);

    physics->RemoveBody(first);
    physics->RemoveBody(second);
#else
    (void)hr;
#endif
    physics->Shutdown();
}
