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
