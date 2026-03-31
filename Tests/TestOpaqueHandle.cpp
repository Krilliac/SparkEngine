// TestOpaqueHandle.cpp - Tests for type-safe opaque handle wrapper

#include "TestFramework.h"
#include "Utils/OpaqueHandle.h"

TEST(OpaqueHandle_DefaultIsInvalid)
{
    Spark::PhysicsHandle handle;
    EXPECT_FALSE(handle.IsValid());
    EXPECT_FALSE(static_cast<bool>(handle));
    EXPECT_EQ(handle.Raw(), nullptr);
}

TEST(OpaqueHandle_ExplicitConstruction)
{
    int dummy = 42;
    Spark::PhysicsHandle handle(static_cast<void*>(&dummy));
    EXPECT_TRUE(handle.IsValid());
    EXPECT_TRUE(static_cast<bool>(handle));
    EXPECT_NE(handle.Raw(), nullptr);
}

TEST(OpaqueHandle_AsTypeCast)
{
    int value = 99;
    Spark::PhysicsHandle handle(static_cast<void*>(&value));
    int* ptr = handle.As<int>();
    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(*ptr, 99);
}

TEST(OpaqueHandle_Reset)
{
    int dummy = 0;
    Spark::AudioHandle handle(static_cast<void*>(&dummy));
    EXPECT_TRUE(handle.IsValid());
    handle.Reset();
    EXPECT_FALSE(handle.IsValid());
    EXPECT_EQ(handle.Raw(), nullptr);
}

TEST(OpaqueHandle_NullptrAssignment)
{
    int dummy = 0;
    Spark::ParticleHandle handle(static_cast<void*>(&dummy));
    EXPECT_TRUE(handle.IsValid());
    handle = nullptr;
    EXPECT_FALSE(handle.IsValid());
}

TEST(OpaqueHandle_Equality)
{
    int a = 1, b = 2;
    Spark::AnimationHandle h1(static_cast<void*>(&a));
    Spark::AnimationHandle h2(static_cast<void*>(&a));
    Spark::AnimationHandle h3(static_cast<void*>(&b));

    EXPECT_TRUE(h1 == h2);
    EXPECT_FALSE(h1 == h3);
    EXPECT_TRUE(h1 != h3);
    EXPECT_FALSE(h1 != h2);
}

TEST(OpaqueHandle_DifferentTagTypesAreDistinct)
{
    // This is a compile-time test — PhysicsHandle and AudioHandle are
    // different types, so they cannot be compared or assigned to each other.
    // We verify they can coexist independently.
    int dummy = 0;
    Spark::PhysicsHandle ph(static_cast<void*>(&dummy));
    Spark::AudioHandle ah(static_cast<void*>(&dummy));
    EXPECT_TRUE(ph.IsValid());
    EXPECT_TRUE(ah.IsValid());
    EXPECT_EQ(ph.Raw(), ah.Raw());
}

// =============================================================================
// All Handle Aliases
// =============================================================================

TEST(OpaqueHandle_ParticleHandle)
{
    int dummy = 0;
    Spark::ParticleHandle h(static_cast<void*>(&dummy));
    EXPECT_TRUE(h.IsValid());
    EXPECT_EQ(h.As<int>(), &dummy);
    h = nullptr;
    EXPECT_FALSE(h.IsValid());
}

TEST(OpaqueHandle_BehaviorTreeHandle)
{
    int dummy = 0;
    Spark::BehaviorTreeHandle h(static_cast<void*>(&dummy));
    EXPECT_TRUE(h.IsValid());
    h.Reset();
    EXPECT_FALSE(h.IsValid());
}

TEST(OpaqueHandle_NavQueryHandle)
{
    float dummy = 3.14f;
    Spark::NavQueryHandle h(static_cast<void*>(&dummy));
    EXPECT_TRUE(h.IsValid());
    float* p = h.As<float>();
    EXPECT_NE(p, nullptr);
    EXPECT_NEAR(*p, 3.14f, 0.01f);
}

// =============================================================================
// Default handle comparisons
// =============================================================================

TEST(OpaqueHandle_DefaultEquality)
{
    Spark::PhysicsHandle h1;
    Spark::PhysicsHandle h2;
    EXPECT_TRUE(h1 == h2);
    EXPECT_FALSE(h1 != h2);
}

TEST(OpaqueHandle_ValidVsInvalid)
{
    int dummy = 0;
    Spark::PhysicsHandle valid(static_cast<void*>(&dummy));
    Spark::PhysicsHandle invalid;
    EXPECT_TRUE(valid != invalid);
    EXPECT_FALSE(valid == invalid);
}

// =============================================================================
// Boolean conversion
// =============================================================================

TEST(OpaqueHandle_BoolConversion)
{
    Spark::AudioHandle h;
    if (h)
    {
        EXPECT_TRUE(false); // Should not reach here
    }
    else
    {
        EXPECT_TRUE(true);
    }

    int dummy = 0;
    Spark::AudioHandle h2(static_cast<void*>(&dummy));
    if (h2)
    {
        EXPECT_TRUE(true);
    }
    else
    {
        EXPECT_TRUE(false);
    }
}
