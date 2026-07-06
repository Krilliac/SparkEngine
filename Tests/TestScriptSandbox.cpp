/**
 * @file TestScriptSandbox.cpp
 * @brief Tests for the ScriptSandbox execution limiter
 */

#include "TestFramework.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// ============================================================================
// Inline minimal ScriptSandbox reproduction for unit testing
// ============================================================================

namespace TestSandbox
{

    enum class ScriptSecurityLevel : uint8_t
    {
        Unrestricted,
        Standard,
        Strict
    };

    enum class SandboxViolationType : uint8_t
    {
        InstructionLimit,
        TimeoutLimit,
        MemoryLimit,
        BlockedFunction
    };

    struct SandboxViolation
    {
        SandboxViolationType type;
        std::string scriptName;
        std::string details;
    };

    class ScriptSandbox
    {
      public:
        ScriptSandbox() { ApplySecurityDefaults(ScriptSecurityLevel::Standard); }

        void SetSecurityLevel(ScriptSecurityLevel level)
        {
            m_securityLevel = level;
            ApplySecurityDefaults(level);
        }

        ScriptSecurityLevel GetSecurityLevel() const { return m_securityLevel; }

        void SetInstructionLimit(uint32_t max) { m_maxInstructions = max; }
        void SetExecutionTimeout(float sec) { m_maxExecutionTimeSec = sec; }
        void SetMemoryLimit(size_t bytes) { m_maxMemoryBytes = bytes; }

        void AddAllowedFunction(const std::string& name) { m_allowedFunctions.insert(name); }
        void AddBlockedFunction(const std::string& name) { m_blockedFunctions.insert(name); }
        void RemoveAllowedFunction(const std::string& name) { m_allowedFunctions.erase(name); }
        void RemoveBlockedFunction(const std::string& name) { m_blockedFunctions.erase(name); }

        bool IsFunctionAllowed(const std::string& name) const
        {
            switch (m_securityLevel)
            {
            case ScriptSecurityLevel::Unrestricted:
                return true;
            case ScriptSecurityLevel::Standard:
                return m_blockedFunctions.find(name) == m_blockedFunctions.end();
            case ScriptSecurityLevel::Strict:
                return m_allowedFunctions.find(name) != m_allowedFunctions.end();
            }
            return false;
        }

        void BeginExecution(const std::string& scriptName)
        {
            m_currentScript = scriptName;
            m_instructionCount = 0;
            m_wasTerminated = false;
            m_executionStart = std::chrono::steady_clock::now();
        }

        void EndExecution() { m_currentScript.clear(); }
        bool WasTerminated() const { return m_wasTerminated; }

        void TrackAllocation(size_t bytes)
        {
            m_currentMemoryUsage += bytes;
            if (m_maxMemoryBytes > 0 && m_currentMemoryUsage > m_maxMemoryBytes)
            {
                m_wasTerminated = true;
                RecordViolation(SandboxViolationType::MemoryLimit, "Memory limit exceeded");
            }
        }

        void TrackDeallocation(size_t bytes)
        {
            m_currentMemoryUsage = (bytes <= m_currentMemoryUsage) ? m_currentMemoryUsage - bytes : 0;
        }

        size_t GetCurrentMemoryUsage() const { return m_currentMemoryUsage; }

        bool SimulateLineCallback()
        {
            m_instructionCount += 1000;
            if (m_maxInstructions > 0 && m_instructionCount >= m_maxInstructions)
            {
                m_wasTerminated = true;
                RecordViolation(SandboxViolationType::InstructionLimit, "Instruction limit reached");
                return false;
            }
            if (m_maxExecutionTimeSec > 0)
            {
                auto elapsed = std::chrono::steady_clock::now() - m_executionStart;
                float elapsedSec = std::chrono::duration<float>(elapsed).count();
                if (elapsedSec >= m_maxExecutionTimeSec)
                {
                    m_wasTerminated = true;
                    RecordViolation(SandboxViolationType::TimeoutLimit, "Execution timeout");
                    return false;
                }
            }
            return true;
        }

        std::vector<SandboxViolation> GetViolations() const
        {
            std::lock_guard<std::mutex> lock(m_violationMutex);
            return m_violations;
        }

        uint32_t GetTotalViolations() const
        {
            std::lock_guard<std::mutex> lock(m_violationMutex);
            return m_totalViolations;
        }

        void ClearViolations()
        {
            std::lock_guard<std::mutex> lock(m_violationMutex);
            m_violations.clear();
            m_totalViolations = 0;
        }

      private:
        ScriptSecurityLevel m_securityLevel = ScriptSecurityLevel::Standard;
        uint32_t m_maxInstructions = 1'000'000;
        float m_maxExecutionTimeSec = 0.1f;
        size_t m_maxMemoryBytes = 16 * 1024 * 1024;
        uint32_t m_instructionCount = 0;
        std::chrono::steady_clock::time_point m_executionStart;
        std::string m_currentScript;
        bool m_wasTerminated = false;
        size_t m_currentMemoryUsage = 0;
        std::unordered_set<std::string> m_allowedFunctions;
        std::unordered_set<std::string> m_blockedFunctions;
        mutable std::mutex m_violationMutex;
        std::vector<SandboxViolation> m_violations;
        uint32_t m_totalViolations = 0;

        void RecordViolation(SandboxViolationType type, const std::string& details)
        {
            std::lock_guard<std::mutex> lock(m_violationMutex);
            m_totalViolations++;
            m_violations.push_back({type, m_currentScript, details});
        }

        void ApplySecurityDefaults(ScriptSecurityLevel level)
        {
            switch (level)
            {
            case ScriptSecurityLevel::Unrestricted:
                m_maxInstructions = 0;
                m_maxExecutionTimeSec = 0;
                m_maxMemoryBytes = 0;
                break;
            case ScriptSecurityLevel::Standard:
                m_maxInstructions = 1'000'000;
                m_maxExecutionTimeSec = 0.1f;
                m_maxMemoryBytes = 16 * 1024 * 1024;
                break;
            case ScriptSecurityLevel::Strict:
                m_maxInstructions = 100'000;
                m_maxExecutionTimeSec = 0.05f;
                m_maxMemoryBytes = 4 * 1024 * 1024;
                break;
            }
        }
    };

} // namespace TestSandbox

// ============================================================================
// Tests
// ============================================================================

TEST(ScriptSandbox_DefaultSecurityLevel)
{
    TestSandbox::ScriptSandbox sandbox;
    EXPECT_TRUE(sandbox.GetSecurityLevel() == TestSandbox::ScriptSecurityLevel::Standard);
}

TEST(ScriptSandbox_SetSecurityLevel)
{
    TestSandbox::ScriptSandbox sandbox;

    sandbox.SetSecurityLevel(TestSandbox::ScriptSecurityLevel::Unrestricted);
    EXPECT_TRUE(sandbox.GetSecurityLevel() == TestSandbox::ScriptSecurityLevel::Unrestricted);

    sandbox.SetSecurityLevel(TestSandbox::ScriptSecurityLevel::Strict);
    EXPECT_TRUE(sandbox.GetSecurityLevel() == TestSandbox::ScriptSecurityLevel::Strict);
}

TEST(ScriptSandbox_InstructionLimitTermination)
{
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetInstructionLimit(5000);

    sandbox.BeginExecution("TestScript::Update");

    bool result = true;
    for (int i = 0; i < 10; ++i)
    {
        result = sandbox.SimulateLineCallback();
        if (!result)
            break;
    }

    EXPECT_FALSE(result);
    EXPECT_TRUE(sandbox.WasTerminated());
    EXPECT_EQ(sandbox.GetTotalViolations(), 1u);

    auto violations = sandbox.GetViolations();
    EXPECT_EQ(violations.size(), 1u);
    EXPECT_TRUE(violations[0].type == TestSandbox::SandboxViolationType::InstructionLimit);
    EXPECT_EQ(violations[0].scriptName, std::string("TestScript::Update"));
}

TEST(ScriptSandbox_NoTerminationWithinLimits)
{
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetInstructionLimit(100'000);
    sandbox.SetExecutionTimeout(10.0f);

    sandbox.BeginExecution("SafeScript");

    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(sandbox.SimulateLineCallback());
    }

    EXPECT_FALSE(sandbox.WasTerminated());
    EXPECT_EQ(sandbox.GetTotalViolations(), 0u);
}

TEST(ScriptSandbox_MemoryLimitTracking)
{
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetMemoryLimit(1024);

    sandbox.BeginExecution("MemTest");

    sandbox.TrackAllocation(512);
    EXPECT_EQ(sandbox.GetCurrentMemoryUsage(), 512u);
    EXPECT_FALSE(sandbox.WasTerminated());

    sandbox.TrackAllocation(1024);
    EXPECT_TRUE(sandbox.WasTerminated());
    EXPECT_EQ(sandbox.GetTotalViolations(), 1u);
}

TEST(ScriptSandbox_MemoryDeallocation)
{
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetMemoryLimit(0);

    sandbox.TrackAllocation(1000);
    EXPECT_EQ(sandbox.GetCurrentMemoryUsage(), 1000u);

    sandbox.TrackDeallocation(400);
    EXPECT_EQ(sandbox.GetCurrentMemoryUsage(), 600u);

    sandbox.TrackDeallocation(1000);
    EXPECT_EQ(sandbox.GetCurrentMemoryUsage(), 0u);
}

TEST(ScriptSandbox_UnrestrictedAllowsAll)
{
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetSecurityLevel(TestSandbox::ScriptSecurityLevel::Unrestricted);

    EXPECT_TRUE(sandbox.IsFunctionAllowed("destroyEntity"));
    EXPECT_TRUE(sandbox.IsFunctionAllowed("print"));
    EXPECT_TRUE(sandbox.IsFunctionAllowed("anything"));
}

TEST(ScriptSandbox_StandardBlacklist)
{
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetSecurityLevel(TestSandbox::ScriptSecurityLevel::Standard);

    EXPECT_TRUE(sandbox.IsFunctionAllowed("print"));
    EXPECT_TRUE(sandbox.IsFunctionAllowed("getTransform"));

    sandbox.AddBlockedFunction("destroyEntity");
    EXPECT_FALSE(sandbox.IsFunctionAllowed("destroyEntity"));
    EXPECT_TRUE(sandbox.IsFunctionAllowed("print"));

    sandbox.RemoveBlockedFunction("destroyEntity");
    EXPECT_TRUE(sandbox.IsFunctionAllowed("destroyEntity"));
}

TEST(ScriptSandbox_StrictWhitelist)
{
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetSecurityLevel(TestSandbox::ScriptSecurityLevel::Strict);

    EXPECT_FALSE(sandbox.IsFunctionAllowed("print"));
    EXPECT_FALSE(sandbox.IsFunctionAllowed("destroyEntity"));

    sandbox.AddAllowedFunction("print");
    EXPECT_TRUE(sandbox.IsFunctionAllowed("print"));
    EXPECT_FALSE(sandbox.IsFunctionAllowed("destroyEntity"));

    sandbox.RemoveAllowedFunction("print");
    EXPECT_FALSE(sandbox.IsFunctionAllowed("print"));
}

TEST(ScriptSandbox_ClearViolations)
{
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetInstructionLimit(1000);

    sandbox.BeginExecution("FailScript");
    sandbox.SimulateLineCallback();
    sandbox.EndExecution();

    EXPECT_EQ(sandbox.GetTotalViolations(), 1u);

    sandbox.ClearViolations();
    EXPECT_EQ(sandbox.GetTotalViolations(), 0u);
    EXPECT_TRUE(sandbox.GetViolations().empty());
}

TEST(ScriptSandbox_UnrestrictedNoInstructionLimit)
{
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetSecurityLevel(TestSandbox::ScriptSecurityLevel::Unrestricted);

    sandbox.BeginExecution("UnlimitedScript");

    for (int i = 0; i < 1000; ++i)
    {
        EXPECT_TRUE(sandbox.SimulateLineCallback());
    }

    EXPECT_FALSE(sandbox.WasTerminated());
}

// ============================================================================
// Registration-guard decision logic
//
// AngelScriptEngine::RegisterGuardedFunction (AngelScriptEngine.cpp) gates
// every RegisterGlobalFunction call site: a native function is bound to the
// AngelScript engine iff sandbox->IsFunctionAllowed(scriptVisibleName) is
// true; otherwise it is silently skipped, so a script referencing it fails
// at BuildModule() time with an undefined-symbol compile error. That is the
// fix for the bug this test file covers: IsFunctionAllowed was fully
// implemented but had zero call sites, so Strict mode provided no actual
// whitelist enforcement.
//
// INTEGRATION GAP: TestAngelScriptEngine.cpp (this repo's convention for
// this subsystem) already uses a standalone reimplementation rather than the
// real AngelScriptEngine class, because SparkTests does not link the real
// AngelScript SDK — the production SPARK_ANGELSCRIPT_SUPPORT macro is (as of
// this change) not defined anywhere in CMakeLists.txt, so the real
// RegisterGuardedFunction/Initialize() code path is not compiled into any
// current build target, test or production. A true end-to-end test (compile
// a script under Strict with a function excluded from the whitelist and
// assert BuildModule() fails; assert it succeeds once whitelisted) requires
// wiring the AngelScript SDK into the build first. Until then, this
// exercises the exact decision rule RegisterGuardedFunction applies —
// WouldRegisterFunction below is that rule, reproduced verbatim.
// ============================================================================

namespace TestSandbox
{
    // Mirrors AngelScriptEngine::RegisterGuardedFunction's gating decision:
    // a function is registered with the AngelScript engine iff the sandbox
    // allows it. Returns what the guard would decide, without needing an
    // actual asIScriptEngine to register against.
    bool WouldRegisterFunction(const ScriptSandbox& sandbox, const std::string& scriptVisibleName)
    {
        return sandbox.IsFunctionAllowed(scriptVisibleName);
    }
} // namespace TestSandbox

TEST(ScriptSandbox_RegistrationGuard_PermissiveAllowsNormalFunction)
{
    // (a) No regression: under the default/permissive level (Standard, no
    // blacklist), a normal engine function must still be registered so
    // existing scripts and tests keep working unchanged.
    TestSandbox::ScriptSandbox sandbox;
    EXPECT_TRUE(sandbox.GetSecurityLevel() == TestSandbox::ScriptSecurityLevel::Standard);

    EXPECT_TRUE(TestSandbox::WouldRegisterFunction(sandbox, "destroyEntity"));
    EXPECT_TRUE(TestSandbox::WouldRegisterFunction(sandbox, "print"));
    EXPECT_TRUE(TestSandbox::WouldRegisterFunction(sandbox, "getComponentField"));
    EXPECT_TRUE(TestSandbox::WouldRegisterFunction(sandbox, "setComponentField"));
}

TEST(ScriptSandbox_RegistrationGuard_StrictBlocksNonWhitelisted)
{
    // (b) The gate works: Strict + a whitelist that excludes "destroyEntity"
    // must refuse to register it — equivalent to a script that calls
    // destroyEntity() failing BuildModule() with an undefined-symbol error.
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetSecurityLevel(TestSandbox::ScriptSecurityLevel::Strict);
    sandbox.AddAllowedFunction("print");

    EXPECT_TRUE(TestSandbox::WouldRegisterFunction(sandbox, "print"));
    EXPECT_FALSE(TestSandbox::WouldRegisterFunction(sandbox, "destroyEntity"));
    EXPECT_FALSE(TestSandbox::WouldRegisterFunction(sandbox, "setComponentField"));
}

TEST(ScriptSandbox_RegistrationGuard_StrictAllowsWhitelisted)
{
    // (c) Strict + the function whitelisted -> the guard registers it.
    TestSandbox::ScriptSandbox sandbox;
    sandbox.SetSecurityLevel(TestSandbox::ScriptSecurityLevel::Strict);
    sandbox.AddAllowedFunction("destroyEntity");

    EXPECT_TRUE(TestSandbox::WouldRegisterFunction(sandbox, "destroyEntity"));
    EXPECT_FALSE(TestSandbox::WouldRegisterFunction(sandbox, "print"));
}
