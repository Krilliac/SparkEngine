/**
 * @file TestScriptSandbox.cpp
 * @brief Real-class tests for Spark::ScriptSandbox.
 *
 * Previously this file exercised a standalone TestSandbox::ScriptSandbox
 * reimplementation that had already diverged from the shipped class (its mock
 * memory tracking flipped WasTerminated where the real one does not). Those
 * tests validated the copy, not the engine, so a regression in ScriptSandbox
 * would not fail them. This version links the real ScriptSandbox.cpp and
 * asserts against the AngelScript-independent surface: security-level
 * round-trip, the three IsFunctionAllowed policies, allow/block list mutation,
 * and the violation bookkeeping accessors.
 */

#include "TestFramework.h"
#include "Engine/Scripting/ScriptSandbox.h"

using Spark::ScriptSandbox;
using Spark::ScriptSecurityLevel;

TEST(ScriptSandboxReal_SecurityLevelRoundTrip)
{
    ScriptSandbox sb;
    sb.SetSecurityLevel(ScriptSecurityLevel::Strict);
    EXPECT_TRUE(sb.GetSecurityLevel() == ScriptSecurityLevel::Strict);

    sb.SetSecurityLevel(ScriptSecurityLevel::Unrestricted);
    EXPECT_TRUE(sb.GetSecurityLevel() == ScriptSecurityLevel::Unrestricted);

    sb.SetSecurityLevel(ScriptSecurityLevel::Standard);
    EXPECT_TRUE(sb.GetSecurityLevel() == ScriptSecurityLevel::Standard);
}

TEST(ScriptSandboxReal_UnrestrictedAllowsEverything)
{
    ScriptSandbox sb;
    sb.SetSecurityLevel(ScriptSecurityLevel::Unrestricted);
    // Even an explicitly blocked name is allowed when unrestricted.
    sb.AddBlockedFunction("system");
    EXPECT_TRUE(sb.IsFunctionAllowed("system"));
    EXPECT_TRUE(sb.IsFunctionAllowed("anything_at_all"));
}

TEST(ScriptSandboxReal_StandardIsBlacklist)
{
    ScriptSandbox sb;
    sb.SetSecurityLevel(ScriptSecurityLevel::Standard);
    // A custom, un-blocked name is allowed under the blacklist policy.
    EXPECT_TRUE(sb.IsFunctionAllowed("myCustomFn_unblocked"));

    sb.AddBlockedFunction("myCustomFn_unblocked");
    EXPECT_FALSE(sb.IsFunctionAllowed("myCustomFn_unblocked"));

    sb.RemoveBlockedFunction("myCustomFn_unblocked");
    EXPECT_TRUE(sb.IsFunctionAllowed("myCustomFn_unblocked"));
}

TEST(ScriptSandboxReal_StrictIsWhitelist)
{
    ScriptSandbox sb;
    sb.SetSecurityLevel(ScriptSecurityLevel::Strict);
    // Under the whitelist policy an unknown name is denied.
    EXPECT_FALSE(sb.IsFunctionAllowed("myStrictFn_xyz"));

    sb.AddAllowedFunction("myStrictFn_xyz");
    EXPECT_TRUE(sb.IsFunctionAllowed("myStrictFn_xyz"));
    // A different name is still denied.
    EXPECT_FALSE(sb.IsFunctionAllowed("someOtherFn"));

    sb.RemoveAllowedFunction("myStrictFn_xyz");
    EXPECT_FALSE(sb.IsFunctionAllowed("myStrictFn_xyz"));
}

TEST(ScriptSandboxReal_ViolationsInitiallyEmpty)
{
    ScriptSandbox sb;
    EXPECT_EQ(sb.GetTotalViolations(), static_cast<uint32_t>(0));
    EXPECT_TRUE(sb.GetViolations().empty());

    sb.ClearViolations();
    EXPECT_EQ(sb.GetTotalViolations(), static_cast<uint32_t>(0));
    EXPECT_TRUE(sb.GetViolations().empty());
}

TEST(ScriptSandboxReal_NotTerminatedWithoutExecution)
{
    ScriptSandbox sb;
    sb.SetSecurityLevel(ScriptSecurityLevel::Standard);
    sb.BeginExecution("test_script");
    sb.EndExecution();
    // No AngelScript context ran, so the sandbox never terminates anything.
    EXPECT_FALSE(sb.WasTerminated());
}

TEST(ScriptSandboxReal_StatusStringNonEmpty)
{
    ScriptSandbox sb;
    EXPECT_TRUE(!sb.GetStatusString().empty());
}
