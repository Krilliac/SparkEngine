/**
 * @file TestContracts.cpp
 * @brief Tests for C++26 contract bridge macros (Contracts.h)
 */

#include "TestFramework.h"

#include "Core/Contracts.h"

// ============================================================================
// SPARK_EXPECTS — precondition macro
// ============================================================================

TEST(Contracts_ExpectsPassesOnTrueCondition)
{
    // Should not fire — condition is true
    SPARK_EXPECTS(1 + 1 == 2);
    EXPECT_TRUE(true); // reached without assertion
}

TEST(Contracts_ExpectsAcceptsComplexExpressions)
{
    int value = 42;
    SPARK_EXPECTS(value > 0 && value < 100);
    EXPECT_TRUE(true);
}

// ============================================================================
// SPARK_ENSURES — postcondition macro
// ============================================================================

TEST(Contracts_EnsuresPassesOnTrueCondition)
{
    int result = 10;
    SPARK_ENSURES(result == 10);
    EXPECT_TRUE(true);
}

TEST(Contracts_EnsuresAcceptsPointerChecks)
{
    int x = 5;
    int* ptr = &x;
    SPARK_ENSURES(ptr != nullptr);
    EXPECT_TRUE(true);
}

// ============================================================================
// SPARK_INVARIANT — invariant macro
// ============================================================================

TEST(Contracts_InvariantPassesOnTrueCondition)
{
    bool initialized = true;
    SPARK_INVARIANT(initialized);
    EXPECT_TRUE(true);
}

TEST(Contracts_AllThreeMacrosInOneFunction)
{
    int input = 5;
    SPARK_EXPECTS(input > 0);

    int output = input * 2;
    SPARK_INVARIANT(output > input);
    SPARK_ENSURES(output == 10);

    EXPECT_EQ(output, 10);
}
