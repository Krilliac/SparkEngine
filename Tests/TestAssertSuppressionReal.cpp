/**
 * @file TestAssertSuppressionReal.cpp
 * @brief Real-class tests for Assert.h suppression API
 */

#include "TestFramework.h"
#include "Utils/Assert.h"

// ============================================================================
// Suppression toggle
// ============================================================================

TEST(AssertSuppressionReal_DefaultOff)
{
    // Ensure suppression is off to start
    Assert::SetSuppressionEnabled(false);
    EXPECT_FALSE(Assert::IsSuppressionEnabled());
}

TEST(AssertSuppressionReal_EnableDisable)
{
    Assert::SetSuppressionEnabled(true);
    EXPECT_TRUE(Assert::IsSuppressionEnabled());
    Assert::SetSuppressionEnabled(false);
    EXPECT_FALSE(Assert::IsSuppressionEnabled());
}

// ============================================================================
// Suppressed count tracking
// ============================================================================

TEST(AssertSuppressionReal_ResetSuppressedCount)
{
    Assert::SetSuppressionEnabled(false);
    Assert::ResetSuppressedCount();
    EXPECT_EQ(Assert::GetSuppressedCount(), 0u);
}

TEST(AssertSuppressionReal_SuppressedAssertCountsUp)
{
    // Enable suppression, disable debug break so we don't trap
    Assert::SetSuppressionEnabled(true);
    Assert::SetDebugBreakOnSuppressed(false);
    Assert::ResetSuppressedCount();

    // Fire a suppressed assertion via VERIFY (always active)
    VERIFY(false); // This will be suppressed, not abort

    EXPECT_GE(Assert::GetSuppressedCount(), 1u);

    // Clean up
    Assert::SetSuppressionEnabled(false);
    Assert::SetDebugBreakOnSuppressed(true);
    Assert::ResetSuppressedCount();
}

// ============================================================================
// Debug break on suppressed toggle
// ============================================================================

TEST(AssertSuppressionReal_DebugBreakToggle)
{
    // Just verify the API doesn't crash; we can't observe the internal flag
    // directly but we can toggle it safely.
    EXPECT_NO_THROW(Assert::SetDebugBreakOnSuppressed(false));
    EXPECT_NO_THROW(Assert::SetDebugBreakOnSuppressed(true));
}

// ============================================================================
// Macro presence (compile-time checks)
// ============================================================================

TEST(AssertSuppressionReal_VerifyMacroExists)
{
    // VERIFY is always active -- passing true should not abort
    VERIFY(true);
    EXPECT_TRUE(true); // If we reached here, VERIFY(true) didn't abort
}

TEST(AssertSuppressionReal_AssertAlwaysMacroExists)
{
#if !defined(DISABLE_ALWAYS_ASSERTS)
    ASSERT_ALWAYS(true);
#endif
    EXPECT_TRUE(true); // If we reached here, ASSERT_ALWAYS(true) didn't abort
}

TEST(AssertSuppressionReal_MultipleSuppressedFires)
{
    Assert::SetSuppressionEnabled(true);
    Assert::SetDebugBreakOnSuppressed(false);
    Assert::ResetSuppressedCount();

    VERIFY(false);
    VERIFY(false);
    VERIFY(false);

    EXPECT_GE(Assert::GetSuppressedCount(), 3u);

    Assert::SetSuppressionEnabled(false);
    Assert::SetDebugBreakOnSuppressed(true);
    Assert::ResetSuppressedCount();
}
