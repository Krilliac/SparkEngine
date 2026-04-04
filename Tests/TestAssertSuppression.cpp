// TestAssertSuppression.cpp - Tests for assertion suppression toggle
// Tests the Assert::SetSuppressionEnabled/IsSuppressionEnabled/GetSuppressedCount API

#include "TestFramework.h"
#include <cstdint>

// Forward declarations matching Assert.h suppression API
namespace Assert
{
    void SetSuppressionEnabled(bool enabled);
    bool IsSuppressionEnabled();
    uint32_t GetSuppressedCount();
    void ResetSuppressedCount();
    void SetDebugBreakOnSuppressed(bool enabled);
    void Fail(const char* expr, const char* file, int line, const char* fmt = nullptr, ...);
} // namespace Assert

// =============================================================================
// Basic API tests
// =============================================================================

TEST(AssertSuppression_DefaultOff)
{
    // Suppression must be OFF by default
    EXPECT_FALSE(Assert::IsSuppressionEnabled());
}

TEST(AssertSuppression_EnableDisable)
{
    // Enable suppression
    Assert::SetSuppressionEnabled(true);
    EXPECT_TRUE(Assert::IsSuppressionEnabled());

    // Disable suppression
    Assert::SetSuppressionEnabled(false);
    EXPECT_FALSE(Assert::IsSuppressionEnabled());
}

TEST(AssertSuppression_CounterStartsAtZero)
{
    Assert::ResetSuppressedCount();
    EXPECT_EQ(Assert::GetSuppressedCount(), 0u);
}

TEST(AssertSuppression_FailIncrementsCounter)
{
    // Enable suppression and disable debug break so we don't hit breakpoints
    Assert::SetSuppressionEnabled(true);
    Assert::SetDebugBreakOnSuppressed(false);
    Assert::ResetSuppressedCount();

    // Trigger a suppressed assertion
    Assert::Fail("test_expr", __FILE__, __LINE__, "test suppression");
    EXPECT_EQ(Assert::GetSuppressedCount(), 1u);

    // Trigger another
    Assert::Fail("test_expr2", __FILE__, __LINE__);
    EXPECT_EQ(Assert::GetSuppressedCount(), 2u);

    // Clean up
    Assert::SetSuppressionEnabled(false);
    Assert::SetDebugBreakOnSuppressed(true);
}

TEST(AssertSuppression_ResetCount)
{
    Assert::SetSuppressionEnabled(true);
    Assert::SetDebugBreakOnSuppressed(false);
    Assert::ResetSuppressedCount();

    Assert::Fail("test_expr", __FILE__, __LINE__);
    EXPECT_GT(Assert::GetSuppressedCount(), 0u);

    Assert::ResetSuppressedCount();
    EXPECT_EQ(Assert::GetSuppressedCount(), 0u);

    // Clean up
    Assert::SetSuppressionEnabled(false);
    Assert::SetDebugBreakOnSuppressed(true);
}

TEST(AssertSuppression_FailReturnsWhenSuppressed)
{
    // The key test: Fail() must return (not abort) when suppression is on
    Assert::SetSuppressionEnabled(true);
    Assert::SetDebugBreakOnSuppressed(false);
    Assert::ResetSuppressedCount();

    // If Fail() aborts, this test process dies and CTest reports FAIL
    Assert::Fail("should_not_abort", __FILE__, __LINE__, "this should be suppressed");

    // If we get here, suppression worked
    EXPECT_TRUE(true);
    EXPECT_EQ(Assert::GetSuppressedCount(), 1u);

    // Clean up
    Assert::SetSuppressionEnabled(false);
    Assert::SetDebugBreakOnSuppressed(true);
}

TEST(AssertSuppression_MultipleFailsAllSuppressed)
{
    Assert::SetSuppressionEnabled(true);
    Assert::SetDebugBreakOnSuppressed(false);
    Assert::ResetSuppressedCount();

    for (int i = 0; i < 10; ++i)
    {
        Assert::Fail("loop_assert", __FILE__, __LINE__, "iteration %d", i);
    }

    EXPECT_EQ(Assert::GetSuppressedCount(), 10u);

    // Clean up
    Assert::SetSuppressionEnabled(false);
    Assert::SetDebugBreakOnSuppressed(true);
}

TEST(AssertSuppression_DebugBreakToggle)
{
    // Just test the API doesn't crash
    Assert::SetDebugBreakOnSuppressed(false);
    Assert::SetDebugBreakOnSuppressed(true);
    EXPECT_TRUE(true);
}

TEST(AssertSuppression_EnablePrintsBanner)
{
    // Enabling suppression should not crash (banner goes to stderr)
    Assert::SetSuppressionEnabled(true);
    EXPECT_TRUE(Assert::IsSuppressionEnabled());

    // Re-enabling when already enabled should not print again
    Assert::SetSuppressionEnabled(true);
    EXPECT_TRUE(Assert::IsSuppressionEnabled());

    Assert::SetSuppressionEnabled(false);
}
