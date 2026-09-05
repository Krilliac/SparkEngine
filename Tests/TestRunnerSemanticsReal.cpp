/**
 * @file TestRunnerSemanticsReal.cpp
 * @brief Real-source tests for the test runner's own accounting rules.
 *
 * The runner is production code for every gate that reads its output, so its
 * decisions get the same treatment as any other shipped system. This file
 * includes the real TestFramework.h / TestWarnings.h and exercises:
 *   - ClassifyTestOutcome (the [ OK ]/[ EMPTY ]/[ WARN ]/[ FAILED ]/[ SKIP ] rule)
 *   - EXPECT_WARN_ONLY, which waives ONE assertion instead of the whole test
 *   - EXPECT_NO_CRASH, which makes "this only proves we did not crash" countable
 *   - SKIP_TEST, which a compiled-out feature must use instead of a fake pass
 *   - the TestWarnings.h flaky registry lookup
 */

#include "TestFramework.h"
#include "TestWarnings.h"

#include <string>

namespace
{
    TestOutcomeInputs CleanRun()
    {
        TestOutcomeInputs inputs;
        inputs.assertionsRun = 3;
        return inputs;
    }
} // namespace

// ============================================================================
// ClassifyTestOutcome
// ============================================================================

TEST(RunnerSemanticsReal_PassingTestWithAssertionsIsPassed)
{
    EXPECT_TRUE(ClassifyTestOutcome(CleanRun()) == TestOutcome::Passed);
}

TEST(RunnerSemanticsReal_ZeroAssertionTestIsEmptyNotPassed)
{
    TestOutcomeInputs inputs = CleanRun();
    inputs.assertionsRun = 0;
    // The defect this replaces: a test that asserted nothing reported [ OK ]
    // and counted toward the ratchet, so it could only fail by crashing.
    EXPECT_TRUE(ClassifyTestOutcome(inputs) == TestOutcome::Empty);
    EXPECT_FALSE(ClassifyTestOutcome(inputs) == TestOutcome::Passed);
}

TEST(RunnerSemanticsReal_EmptyIsErrorPromotesEmptyToFailure)
{
    TestOutcomeInputs inputs = CleanRun();
    inputs.assertionsRun = 0;
    inputs.emptyIsError = true;
    EXPECT_TRUE(ClassifyTestOutcome(inputs) == TestOutcome::Failed);
}

TEST(RunnerSemanticsReal_EmptyIsErrorDoesNotFailASkippedTest)
{
    TestOutcomeInputs inputs = CleanRun();
    inputs.assertionsRun = 0;
    inputs.skipRequested = true;
    inputs.emptyIsError = true;
    // A SKIP_TEST body legitimately runs no assertions.
    EXPECT_TRUE(ClassifyTestOutcome(inputs) == TestOutcome::Skipped);
}

TEST(RunnerSemanticsReal_HardFailureIsFailedWithoutAWarningPattern)
{
    TestOutcomeInputs inputs = CleanRun();
    inputs.hardFailure = true;
    EXPECT_TRUE(ClassifyTestOutcome(inputs) == TestOutcome::Failed);
}

TEST(RunnerSemanticsReal_NamePatternWaivesTheWholeFailingTest)
{
    TestOutcomeInputs inputs = CleanRun();
    inputs.hardFailure = true;
    inputs.matchedWarningPattern = true;
    EXPECT_TRUE(ClassifyTestOutcome(inputs) == TestOutcome::Warned);
}

TEST(RunnerSemanticsReal_WaivedAssertionAloneWarnsWithoutAPattern)
{
    TestOutcomeInputs inputs = CleanRun();
    inputs.waivedAssertions = 1;
    // EXPECT_WARN_ONLY fired but nothing else failed: the test warns, and it
    // needs no entry in TestWarnings.h to do so.
    EXPECT_TRUE(ClassifyTestOutcome(inputs) == TestOutcome::Warned);
}

TEST(RunnerSemanticsReal_WaivedAssertionDoesNotRescueAHardFailure)
{
    TestOutcomeInputs inputs = CleanRun();
    inputs.waivedAssertions = 1;
    inputs.hardFailure = true;
    // This is the whole point of the per-assertion waiver: a strict assertion
    // failing in the same test must still fail the test.
    EXPECT_TRUE(ClassifyTestOutcome(inputs) == TestOutcome::Failed);
}

TEST(RunnerSemanticsReal_WarnIsErrorPromotesBothWaiverKinds)
{
    TestOutcomeInputs pattern = CleanRun();
    pattern.hardFailure = true;
    pattern.matchedWarningPattern = true;
    pattern.warnIsError = true;
    EXPECT_TRUE(ClassifyTestOutcome(pattern) == TestOutcome::Failed);

    TestOutcomeInputs assertionWaiver = CleanRun();
    assertionWaiver.waivedAssertions = 1;
    assertionWaiver.warnIsError = true;
    EXPECT_TRUE(ClassifyTestOutcome(assertionWaiver) == TestOutcome::Failed);
}

TEST(RunnerSemanticsReal_SkipWinsOverAnUnmatchedCleanRun)
{
    TestOutcomeInputs inputs = CleanRun();
    inputs.skipRequested = true;
    EXPECT_TRUE(ClassifyTestOutcome(inputs) == TestOutcome::Skipped);
}

TEST(RunnerSemanticsReal_SkipDoesNotMaskAHardFailure)
{
    TestOutcomeInputs inputs = CleanRun();
    inputs.skipRequested = true;
    inputs.hardFailure = true;
    EXPECT_TRUE(ClassifyTestOutcome(inputs) == TestOutcome::Failed);
}

// ============================================================================
// EXPECT_WARN_ONLY / EXPECT_NO_CRASH accounting
// ============================================================================

TEST(RunnerSemanticsReal_WarnOnlyPassingAssertionCountsAsAPass)
{
    const int passedBefore = g_assertionsPassed;
    const int waivedBefore = g_assertionsWaived;

    // Non-const locals: a literal condition would be a constant expression and
    // MSVC /W4 rejects those inside an if (C4127).
    int sum = 2 + 2;
    EXPECT_WARN_ONLY(sum == 4, "must not fire");

    EXPECT_EQ(g_assertionsPassed - passedBefore, 1);
    EXPECT_EQ(g_assertionsWaived, waivedBefore);
}

TEST(RunnerSemanticsReal_WarnOnlyFailureIsWaivedNotFailed)
{
    const int passedBefore = g_assertionsPassed;
    const int failedBefore = g_assertionsFailed;
    const int waivedBefore = g_assertionsWaived;

    int one = 1;
    int two = 2;
    EXPECT_WARN_ONLY(one == two, "intentional probe: proves a waiver is not a failure");

    const int passedAfter = g_assertionsPassed;
    const int failedAfter = g_assertionsFailed;
    const int waivedAfter = g_assertionsWaived;

    // Restore the global waiver counter before this test ends. The probe above
    // is deliberately false, and leaving the delta in place would make the
    // runner report this file as a flaky test in every summary and JUnit report.
    g_assertionsWaived = waivedBefore;

    EXPECT_EQ(failedAfter, failedBefore);
    EXPECT_EQ(passedAfter, passedBefore);
    EXPECT_EQ(waivedAfter - waivedBefore, 1);
}

TEST(RunnerSemanticsReal_NoCrashIsACountedAssertionAndIsTallied)
{
    const int passedBefore = g_assertionsPassed;
    const int noCrashBefore = g_assertionsNoCrash;

    EXPECT_NO_CRASH("reaching this line without crashing is the assertion");

    EXPECT_EQ(g_assertionsPassed - passedBefore, 1);
    EXPECT_EQ(g_assertionsNoCrash - noCrashBefore, 1);
}

// ============================================================================
// SKIP_TEST — what a compiled-out feature must report instead of a fake pass
// ============================================================================

TEST(RunnerSemanticsReal_SkipTestThrowsWithItsReason)
{
    bool caught = false;
    std::string reason;
    try
    {
        SKIP_TEST("ENABLE_NETWORKING is OFF in this configuration");
    }
    catch (const TestSkip& skip)
    {
        caught = true;
        reason = skip.reason;
    }

    EXPECT_TRUE(caught);
    EXPECT_STR_CONTAINS(reason, "ENABLE_NETWORKING is OFF");
}

// ============================================================================
// TestWarnings.h registry
// ============================================================================

TEST(RunnerSemanticsReal_WarningRegistryMatchesBySubstring)
{
    // The probe is derived from the registry instead of naming one entry.
    // Removing a waiver from TestWarnings.h is the documented promotion path,
    // and a runner-semantics test must not be collateral damage of it: this
    // test is about the substring rule, not about which tests are waived.
    // (The array cannot legally be empty in C++, so this is a compile-time
    // precondition rather than a runtime branch: a constant condition inside
    // an if() is what MSVC /W4 rejects as C4127.)
    static_assert(g_testWarningPatternCount > 0,
                  "TestWarnings.h must declare at least one pattern for the substring rule to be testable");

    const char* const registered = g_testWarningPatterns[0].pattern;

    EXPECT_TRUE(GetTestWarningReason(registered) != nullptr);
    const std::string decorated = std::string("Prefixed_") + registered + "_Suffixed";
    EXPECT_TRUE(GetTestWarningReason(decorated.c_str()) != nullptr);
    EXPECT_TRUE(GetTestWarningReason("RunnerSemanticsReal_DefinitelyNotRegistered") == nullptr);
}

TEST(RunnerSemanticsReal_EveryWarningEntryCarriesAReason)
{
    EXPECT_GT(g_testWarningPatternCount, static_cast<size_t>(0));
    for (size_t i = 0; i < g_testWarningPatternCount; ++i)
    {
        ASSERT_TRUE(g_testWarningPatterns[i].pattern != nullptr);
        ASSERT_TRUE(g_testWarningPatterns[i].reason != nullptr);
        // An empty pattern would substring-match every test name and silently
        // waive the entire suite; an empty reason hides why it is waived.
        EXPECT_TRUE(g_testWarningPatterns[i].pattern[0] != '\0');
        EXPECT_TRUE(g_testWarningPatterns[i].reason[0] != '\0');
    }
}
