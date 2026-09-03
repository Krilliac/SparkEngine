/**
 * @file TestSafetyCoreUtils.cpp
 * @brief Tests for NonNull pointer wrapper and SafeCast utilities
 */

#include "TestFramework.h"

#include "Core/NonNull.h"
#include "Core/SafeCast.h"

#include <cstdint>
#include <limits>

// ============================================================================
// NonNull<T*> — construction and access
// ============================================================================

TEST(NonNull_ConstructFromValidPointer)
{
    int value = 42;
    Spark::NonNull<int*> ptr(&value);
    EXPECT_EQ(*ptr, 42);
}

TEST(NonNull_ImplicitConversionToRawPointer)
{
    int value = 7;
    Spark::NonNull<int*> nn(&value);

    int* raw = nn; // implicit conversion
    EXPECT_EQ(raw, &value);
}

TEST(NonNull_ArrowOperator)
{
    struct Foo
    {
        int x = 99;
    };
    Foo f;
    Spark::NonNull<Foo*> ptr(&f);
    EXPECT_EQ(ptr->x, 99);
}

TEST(NonNull_DereferenceOperator)
{
    int value = 13;
    Spark::NonNull<int*> ptr(&value);
    EXPECT_EQ(*ptr, 13);

    *ptr = 20;
    EXPECT_EQ(value, 20);
}

TEST(NonNull_GetAccessor)
{
    int value = 5;
    Spark::NonNull<int*> ptr(&value);
    EXPECT_EQ(ptr.Get(), &value);
}

TEST(NonNull_DeductionGuide)
{
    int value = 1;
    Spark::NonNull ptr(&value); // CTAD
    EXPECT_EQ(*ptr, 1);
}

TEST(NonNull_ConstPointer)
{
    const int value = 88;
    Spark::NonNull<const int*> ptr(&value);
    EXPECT_EQ(*ptr, 88);
}

TEST(NonNull_PassToFunctionExpectingRawPointer)
{
    int value = 55;
    Spark::NonNull<int*> nn(&value);

    // Simulate a function accepting raw pointer
    auto readValue = [](int* p) { return *p; };
    EXPECT_EQ(readValue(nn), 55);
}

// ============================================================================
// NarrowCast — safe narrowing conversions
// ============================================================================

TEST(NarrowCast_IntToShortLossless)
{
    int value = 100;
    auto result = Spark::NarrowCast<short>(value);
    EXPECT_EQ(result, 100);
}

TEST(NarrowCast_SizeTToInt)
{
    size_t value = 42;
    auto result = Spark::NarrowCast<int>(value);
    EXPECT_EQ(result, 42);
}

TEST(NarrowCast_DoubleToFloat)
{
    double value = 3.14;
    auto result = Spark::NarrowCast<float>(value);
    // Should succeed — 3.14 is representable as float (close enough for round-trip)
    EXPECT_TRUE(result > 3.13f && result < 3.15f);
}

TEST(NarrowCast_UintToIntPositiveValue)
{
    unsigned int value = 50u;
    auto result = Spark::NarrowCast<int>(value);
    EXPECT_EQ(result, 50);
}

TEST(NarrowCast_IntToUintPositiveValue)
{
    int value = 42;
    auto result = Spark::NarrowCast<unsigned int>(value);
    EXPECT_EQ(result, 42u);
}

TEST(NarrowCast_ZeroRoundTrips)
{
    EXPECT_EQ(Spark::NarrowCast<int>(0.0), 0);
    EXPECT_EQ(Spark::NarrowCast<uint8_t>(0), 0);
    EXPECT_EQ(Spark::NarrowCast<float>(0), 0.0f);
}

// ============================================================================
// CheckedCast — validated pointer downcast
// ============================================================================

namespace
{
    struct Base
    {
        virtual ~Base() = default;
        int baseVal = 1;
    };
    struct Derived : Base
    {
        int derivedVal = 2;
    };
    struct OtherDerived : Base
    {
        int otherVal = 3;
    };
} // namespace

TEST(CheckedCast_ValidDowncast)
{
    Derived d;
    Base* base = &d;
    auto* result = Spark::CheckedCast<Derived*>(base);
    ASSERT_TRUE(result != nullptr);
    EXPECT_EQ(result->derivedVal, 2);
}

TEST(CheckedCast_NullptrPassesThrough)
{
    Base* null = nullptr;
    auto* result = Spark::CheckedCast<Derived*>(null);
    EXPECT_TRUE(result == nullptr);
}

TEST(CheckedCast_ConstCorrectness)
{
    const Derived d;
    const Base* base = &d;
    auto* result = Spark::CheckedCast<const Derived*>(base);
    ASSERT_TRUE(result != nullptr);
    EXPECT_EQ(result->derivedVal, 2);
}
