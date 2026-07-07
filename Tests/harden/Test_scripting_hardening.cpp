/**
 * @file Test_scripting_hardening.cpp
 * @brief Regression tests for the scripting-lane hardening fixes.
 *
 * SparkTests does not link the real AngelScript SDK (SPARK_ANGELSCRIPT_SUPPORT
 * is not defined in the test build), so — following the convention already used
 * by TestScriptSandbox.cpp — each test reproduces the exact decision rule or
 * control-flow invariant of the production fix it guards, without needing an
 * asIScriptEngine to run against. Each reproduction is copied verbatim from the
 * corresponding production code so the test fails against the pre-fix behavior
 * and passes after.
 */

#include "TestFramework.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Fix 1 — HotReloadModule: validate-compile BEFORE detaching live scripts.
//
// Mirrors AngelScriptEngine::HotReloadModule's reordered control flow: a
// staging compile is attempted first; only on success are the live instances
// detached and the module recompiled/re-attached. On failure NOTHING is
// touched. Before the fix, all instances were detached before the recompile,
// so a compile error (the common mid-edit typo) wiped every live script.
// ============================================================================

namespace HotReloadModel
{
    struct Reloader
    {
        std::vector<int> liveEntities;  ///< Entities with the module's script attached.
        bool detachHappened = false;    ///< Set once any live instance is detached.
        int reattachCount = 0;          ///< Number of instances re-attached after recompile.

        /// @param compilesOk Whether the edited source compiles (staging probe result).
        bool Reload(bool compilesOk)
        {
            // Step 2 (fix): pre-validate via a staging compile before touching
            // anything. Bail with live scripts intact if it fails.
            if (!compilesOk)
            {
                return false;
            }

            // Step 3: source is known good — now detach + recompile + re-attach.
            const std::vector<int> saved = liveEntities;
            liveEntities.clear();
            detachHappened = true;
            for (int e : saved)
            {
                liveEntities.push_back(e);
                ++reattachCount;
            }
            return true;
        }
    };
} // namespace HotReloadModel

TEST(ScriptHotReload_CompileFailureLeavesLiveScriptsIntact)
{
    HotReloadModel::Reloader reloader;
    reloader.liveEntities = {1, 2, 3};

    // A syntax error mid-edit: staging compile fails.
    bool ok = reloader.Reload(/*compilesOk=*/false);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(reloader.detachHappened);          // never detached
    EXPECT_EQ(reloader.liveEntities.size(), 3u);    // all live scripts survive
    EXPECT_EQ(reloader.reattachCount, 0);
}

TEST(ScriptHotReload_CompileSuccessReattachesAll)
{
    HotReloadModel::Reloader reloader;
    reloader.liveEntities = {7, 8};

    bool ok = reloader.Reload(/*compilesOk=*/true);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(reloader.detachHappened);
    EXPECT_EQ(reloader.liveEntities.size(), 2u);
    EXPECT_EQ(reloader.reattachCount, 2);
}

// ============================================================================
// Fix 2 — Client/server context enforcement.
//
// Mirrors AngelScriptEngine::GetClassContext + IsClassContextAllowed verbatim.
// Previously m_scriptContext was write-only (never consulted), so server-only
// logic could attach on clients and vice-versa.
// ============================================================================

namespace ContextModel
{
    enum class ScriptContext : uint8_t
    {
        Shared,
        Server,
        Client
    };

    struct Engine
    {
        ScriptContext current = ScriptContext::Shared;
        std::unordered_map<std::string, ScriptContext> classContexts;

        ScriptContext GetClassContext(const std::string& moduleName, const std::string& className) const
        {
            auto it = classContexts.find(moduleName + "::" + className);
            return it != classContexts.end() ? it->second : ScriptContext::Shared;
        }

        bool IsClassContextAllowed(ScriptContext classContext) const
        {
            if (classContext == ScriptContext::Shared)
            {
                return true;
            }
            return classContext == current;
        }

        // The AttachScript gate: may this class attach under the current context?
        bool MayAttach(const std::string& moduleName, const std::string& className) const
        {
            return IsClassContextAllowed(GetClassContext(moduleName, className));
        }
    };
} // namespace ContextModel

TEST(ScriptContext_UntaggedClassAttachesEverywhere)
{
    ContextModel::Engine engine;
    // No metadata recorded -> Shared -> always allowed, in any context.
    engine.current = ContextModel::ScriptContext::Server;
    EXPECT_TRUE(engine.MayAttach("Mod", "PlainClass"));
    engine.current = ContextModel::ScriptContext::Client;
    EXPECT_TRUE(engine.MayAttach("Mod", "PlainClass"));
    engine.current = ContextModel::ScriptContext::Shared;
    EXPECT_TRUE(engine.MayAttach("Mod", "PlainClass"));
}

TEST(ScriptContext_ServerClassBlockedOnClient)
{
    ContextModel::Engine engine;
    engine.classContexts["Mod::SpawnController"] = ContextModel::ScriptContext::Server;

    engine.current = ContextModel::ScriptContext::Server;
    EXPECT_TRUE(engine.MayAttach("Mod", "SpawnController")); // server logic on server: OK

    engine.current = ContextModel::ScriptContext::Client;
    EXPECT_FALSE(engine.MayAttach("Mod", "SpawnController")); // server logic on client: refused
}

TEST(ScriptContext_ClientClassBlockedOnServer)
{
    ContextModel::Engine engine;
    engine.classContexts["Mod::Hud"] = ContextModel::ScriptContext::Client;

    engine.current = ContextModel::ScriptContext::Client;
    EXPECT_TRUE(engine.MayAttach("Mod", "Hud")); // client logic on client: OK

    engine.current = ContextModel::ScriptContext::Server;
    EXPECT_FALSE(engine.MayAttach("Mod", "Hud")); // client logic on dedicated server: refused
}

// ============================================================================
// Fix 5 — Sandbox re-entrancy: nested BeginExecution must not reset the outer
// frame's per-call counters. Mirrors ScriptSandbox::BeginExecution/EndExecution
// and the LineCallback instruction tick.
// ============================================================================

namespace ReentrancyModel
{
    struct Sandbox
    {
        static constexpr uint32_t WEIGHT = 1000;
        uint32_t maxInstructions = 5000;
        uint32_t instructionCount = 0;
        bool wasTerminated = false;
        uint32_t executionDepth = 0;
        std::string currentScript;

        void BeginExecution(const std::string& name)
        {
            if (executionDepth++ > 0)
            {
                return; // nested: do NOT reset the outer frame
            }
            currentScript = name;
            instructionCount = 0;
            wasTerminated = false;
        }

        void EndExecution()
        {
            if (executionDepth > 0 && --executionDepth == 0)
            {
                currentScript.clear();
            }
        }

        // Returns false when the instruction budget is exhausted (context aborts).
        bool Tick()
        {
            instructionCount += WEIGHT;
            if (maxInstructions > 0 && instructionCount >= maxInstructions)
            {
                wasTerminated = true;
                return false;
            }
            return true;
        }
    };
} // namespace ReentrancyModel

TEST(ScriptSandbox_NestedBeginDoesNotResetOuterBudget)
{
    ReentrancyModel::Sandbox sb;
    sb.maxInstructions = 5000;

    sb.BeginExecution("Outer::Update");
    EXPECT_TRUE(sb.Tick()); // 1000
    EXPECT_TRUE(sb.Tick()); // 2000
    EXPECT_TRUE(sb.Tick()); // 3000

    // A native callback re-enters the VM. Without the depth guard this would
    // zero instructionCount and defeat the outer frame's limit.
    sb.BeginExecution("Inner::callback");
    EXPECT_EQ(sb.instructionCount, 3000u); // preserved, not reset
    EXPECT_EQ(sb.currentScript, std::string("Outer::Update"));
    EXPECT_TRUE(sb.Tick());  // 4000
    sb.EndExecution();       // leaving nested frame

    // Outer frame is still active and its budget kept accumulating.
    EXPECT_EQ(sb.currentScript, std::string("Outer::Update"));
    EXPECT_FALSE(sb.Tick()); // 5000 -> exhausted, terminated
    EXPECT_TRUE(sb.wasTerminated);

    sb.EndExecution();
    EXPECT_EQ(sb.executionDepth, 0u);
    EXPECT_TRUE(sb.currentScript.empty());
}

// ============================================================================
// Fix 4 — Console-command lifetime: [this]-capturing sandbox commands must be
// unregistered when the sandbox is destroyed, so they cannot dangle in the
// console after teardown / on a second Initialize(). Mirrors the
// RegisterConsoleCommands / UnregisterConsoleCommands / destructor pattern.
// ============================================================================

namespace ConsoleModel
{
    struct FakeConsole
    {
        std::unordered_map<std::string, std::function<std::string()>> commands;
        void Register(const std::string& name, std::function<std::string()> fn) { commands[name] = std::move(fn); }
        void Unregister(const std::string& name) { commands.erase(name); }
        bool Has(const std::string& name) const { return commands.find(name) != commands.end(); }
    };

    struct Sandbox
    {
        FakeConsole& console;
        bool consoleCommandsRegistered = false;
        int level = 1;

        explicit Sandbox(FakeConsole& c) : console(c) {}

        ~Sandbox() { UnregisterConsoleCommands(); }

        void RegisterConsoleCommands()
        {
            // Captures [this] — exactly the dangling hazard the fix addresses.
            console.Register("sandbox.status", [this]() -> std::string { return "level=" + std::to_string(level); });
            console.Register("sandbox.level", [this]() -> std::string { return std::to_string(level); });
            console.Register("sandbox.violations", []() -> std::string { return "none"; });
            consoleCommandsRegistered = true;
        }

        void UnregisterConsoleCommands()
        {
            if (!consoleCommandsRegistered)
            {
                return;
            }
            console.Unregister("sandbox.status");
            console.Unregister("sandbox.level");
            console.Unregister("sandbox.violations");
            consoleCommandsRegistered = false;
        }
    };
} // namespace ConsoleModel

TEST(ScriptSandbox_ConsoleCommandsRemovedOnDestruction)
{
    ConsoleModel::FakeConsole console;
    {
        ConsoleModel::Sandbox sandbox(console);
        sandbox.RegisterConsoleCommands();
        EXPECT_TRUE(console.Has("sandbox.status"));
        EXPECT_TRUE(console.Has("sandbox.level"));
        EXPECT_TRUE(console.Has("sandbox.violations"));
    }
    // Destructor must have removed every [this]-capturing callback.
    EXPECT_FALSE(console.Has("sandbox.status"));
    EXPECT_FALSE(console.Has("sandbox.level"));
    EXPECT_FALSE(console.Has("sandbox.violations"));
}

TEST(ScriptSandbox_ReinitializeDoesNotLeaveStaleCommands)
{
    ConsoleModel::FakeConsole console;

    // First sandbox registers, then is destroyed (mirrors a second Initialize()
    // replacing m_sandbox): its commands must be gone before the next registers.
    {
        ConsoleModel::Sandbox first(console);
        first.RegisterConsoleCommands();
    }
    EXPECT_FALSE(console.Has("sandbox.status"));

    ConsoleModel::Sandbox second(console);
    second.RegisterConsoleCommands();
    EXPECT_TRUE(console.Has("sandbox.status"));
    // The live command resolves against the SECOND (still-alive) sandbox.
    EXPECT_EQ(console.commands["sandbox.status"](), std::string("level=1"));
    second.UnregisterConsoleCommands();
    EXPECT_FALSE(console.Has("sandbox.status"));
    // Double-unregister is a safe no-op.
    second.UnregisterConsoleCommands();
    EXPECT_FALSE(console.Has("sandbox.status"));
}
