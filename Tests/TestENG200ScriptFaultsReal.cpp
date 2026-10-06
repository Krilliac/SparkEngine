/**
 * @file TestENG200ScriptFaultsReal.cpp
 * @brief ENG-200: real AngelScriptEngine compile/runtime fault handling.
 *
 * Drives the production AngelScriptEngine (not a mirror) through the fault
 * paths the release contract requires: a compile error or a runtime fault
 * must surface an actionable diagnostic that names the script section and
 * line, and a script that faults at runtime (exception, sandbox budget
 * termination) must be disabled instead of being re-executed every frame.
 * Re-attaching the class (the hot-reload path) clears the fault.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "Engine/Scripting/AngelScriptEngine.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

namespace
{
    int g_traceCount = 0;

    void CountTrace(uint32_t /*nodeId*/, const char* /*nodeName*/, const char* /*output*/)
    {
        ++g_traceCount;
    }

    /// Initializes a real engine and routes debugTrace() into g_traceCount.
    struct ScriptFaultFixture
    {
        AngelScriptEngine engine;
        bool ready = false;

        ScriptFaultFixture()
        {
            g_traceCount = 0;
            ASSetDebugTraceCallback(&CountTrace);
            ready = engine.Initialize();
        }

        ~ScriptFaultFixture()
        {
            engine.Shutdown();
            ASSetDebugTraceCallback(nullptr);
        }
    };

    EntityID MakeEntity(uint32_t id)
    {
        return static_cast<EntityID>(id);
    }

    // Line 7 divides by a runtime zero, which AngelScript raises as an exception.
    const char* const kDivideByZeroScript = "class Faulty\n"                        // 1
                                            "{\n"                                   // 2
                                            "    int divisor = 0;\n"                // 3
                                            "    void Update(float dt)\n"           // 4
                                            "    {\n"                               // 5
                                            "        debugTrace(1, \"u\", \"\");\n" // 6
                                            "        int boom = 10 / divisor;\n"    // 7
                                            "    }\n"                               // 8
                                            "    void Start() { debugTrace(2, \"s\", \"\"); }\n"
                                            "    void OnCollision(EntityID other) { debugTrace(3, \"c\", \"\"); }\n"
                                            "}\n";
} // namespace

TEST(ScriptLifecycle_ENG200_RealScriptCompilesAndMovesEntity)
{
    // The engine API registers Vector3-by-value natives (getPosition/getRotation).
    // If that registration is invalid for the platform ABI, AngelScript rejects
    // the whole configuration and no script can compile at all.
    ScriptFaultFixture fx;
    EXPECT_TRUE(fx.ready);

    World world;
    const EntityID mover = world.CreateEntity("Mover");
    world.AddComponent<Transform>(mover);
    AngelScriptEngine::BindWorld(&world);

    const char* const moverScript = "class Mover\n"
                                    "{\n"
                                    "    void Update(float dt)\n"
                                    "    {\n"
                                    "        EntityID self = getEntityByName(\"Mover\");\n"
                                    "        Vector3 p = getPosition(self);\n"
                                    "        p.x += dt;\n"
                                    "        setPosition(self, p);\n"
                                    "    }\n"
                                    "}\n";
    const bool compiled = fx.engine.CompileScriptFromString(moverScript, "ENG200Mover");
    EXPECT_TRUE(compiled);
    if (!compiled)
    {
        std::printf("  compile diagnostic: %s\n", fx.engine.GetLastError().c_str());
    }
    EXPECT_TRUE(fx.engine.AttachScript(mover, "Mover", "ENG200Mover"));

    fx.engine.CallUpdate(mover, 0.5f);
    fx.engine.CallUpdate(mover, 0.5f);

    EXPECT_FALSE(fx.engine.IsScriptFaulted(mover));
    EXPECT_TRUE(std::fabs(world.GetComponent<Transform>(mover)->position.x - 1.0f) < 1e-5f);

    fx.engine.DetachScript(mover);
    AngelScriptEngine::BindWorld(nullptr);
}

TEST(ScriptLifecycle_ENG200_RuntimeExceptionReportsSectionAndLine)
{
    ScriptFaultFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.engine.CompileScriptFromString(kDivideByZeroScript, "ENG200Divide"));
    const EntityID entity = MakeEntity(11);
    EXPECT_TRUE(fx.engine.AttachScript(entity, "Faulty", "ENG200Divide"));

    fx.engine.CallUpdate(entity, 0.016f);

    const std::string error = fx.engine.GetLastError();
    EXPECT_STR_CONTAINS(error, "Update()");
    EXPECT_STR_CONTAINS(error, "Divide by zero");
    EXPECT_STR_CONTAINS(error, "ENG200Divide:7");
    EXPECT_STR_CONTAINS(error, "Faulty");
}

TEST(ScriptLifecycle_ENG200_FaultedScriptIsDisabledNotRerunEveryFrame)
{
    ScriptFaultFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.engine.CompileScriptFromString(kDivideByZeroScript, "ENG200Disable"));
    const EntityID entity = MakeEntity(12);
    EXPECT_TRUE(fx.engine.AttachScript(entity, "Faulty", "ENG200Disable"));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));

    fx.engine.CallUpdate(entity, 0.016f);
    EXPECT_EQ(g_traceCount, 1);
    EXPECT_TRUE(fx.engine.IsScriptFaulted(entity));

    // Subsequent frames and other callbacks must not re-enter the faulted script.
    fx.engine.CallUpdate(entity, 0.016f);
    fx.engine.CallUpdate(entity, 0.016f);
    fx.engine.CallStart(entity);
    fx.engine.CallOnCollision(entity, MakeEntity(99));
    EXPECT_EQ(g_traceCount, 1);
}

TEST(ScriptLifecycle_ENG200_ReattachClearsFaultAndRestoresDispatch)
{
    ScriptFaultFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.engine.CompileScriptFromString(kDivideByZeroScript, "ENG200Reattach"));
    const EntityID entity = MakeEntity(13);
    EXPECT_TRUE(fx.engine.AttachScript(entity, "Faulty", "ENG200Reattach"));
    fx.engine.CallUpdate(entity, 0.016f);
    EXPECT_TRUE(fx.engine.IsScriptFaulted(entity));

    // The fixed source is recompiled and re-attached, exactly what hot reload does.
    const char* const fixedScript = "class Faulty\n"
                                    "{\n"
                                    "    void Update(float dt) { debugTrace(1, \"u\", \"\"); }\n"
                                    "}\n";
    EXPECT_TRUE(fx.engine.CompileScriptFromString(fixedScript, "ENG200Reattach"));
    EXPECT_TRUE(fx.engine.AttachScript(entity, "Faulty", "ENG200Reattach"));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));

    g_traceCount = 0;
    fx.engine.CallUpdate(entity, 0.016f);
    fx.engine.CallUpdate(entity, 0.016f);
    EXPECT_EQ(g_traceCount, 2);
    EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));

    // Detaching forgets the entity entirely.
    fx.engine.DetachScript(entity);
    EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));
}

TEST(ScriptLifecycle_ENG200_RunawayUpdateIsTerminatedWithLineAndDisabled)
{
    ScriptFaultFixture fx;
    EXPECT_TRUE(fx.ready);
    auto* sandbox = fx.engine.GetSandbox();
    EXPECT_TRUE(sandbox != nullptr);
    if (!sandbox)
        return;
    sandbox->SetInstructionLimit(10'000);
    sandbox->SetExecutionTimeout(5.0f);

    const char* const runaway = "class Runaway\n"                       // 1
                                "{\n"                                   // 2
                                "    int spins = 0;\n"                  // 3
                                "    void Update(float dt)\n"           // 4
                                "    {\n"                               // 5
                                "        debugTrace(1, \"u\", \"\");\n" // 6
                                "        while (true)\n"                // 7
                                "        {\n"                           // 8
                                "            spins++;\n"                // 9
                                "        }\n"                           // 10
                                "    }\n"                               // 11
                                "}\n";
    EXPECT_TRUE(fx.engine.CompileScriptFromString(runaway, "ENG200Runaway"));
    const EntityID entity = MakeEntity(14);
    EXPECT_TRUE(fx.engine.AttachScript(entity, "Runaway", "ENG200Runaway"));

    fx.engine.CallUpdate(entity, 0.016f);

    const std::string error = fx.engine.GetLastError();
    EXPECT_STR_CONTAINS(error, "terminated by sandbox");
    EXPECT_STR_CONTAINS(error, "Runaway");
    EXPECT_STR_CONTAINS(error, "ENG200Runaway:");
    EXPECT_TRUE(fx.engine.IsScriptFaulted(entity));

    fx.engine.CallUpdate(entity, 0.016f);
    EXPECT_EQ(g_traceCount, 1);
}

TEST(ScriptLifecycle_ENG200_CompileErrorKeepsCompilerLineDiagnostic)
{
    ScriptFaultFixture fx;
    EXPECT_TRUE(fx.ready);

    const char* const broken = "class Broken\n"                         // 1
                               "{\n"                                    // 2
                               "    void Update(float dt)\n"            // 3
                               "    {\n"                                // 4
                               "        int x = undefinedSymbol + 1;\n" // 5
                               "    }\n"                                // 6
                               "}\n";
    EXPECT_FALSE(fx.engine.CompileScriptFromString(broken, "ENG200Broken"));

    const std::string error = fx.engine.GetLastError();
    EXPECT_STR_CONTAINS(error, "Compilation failed for module 'ENG200Broken'");
    EXPECT_STR_CONTAINS(error, "ENG200Broken:5");
    EXPECT_STR_CONTAINS(error, "undefinedSymbol");

    // A failed compile leaves nothing attachable behind.
    EXPECT_FALSE(fx.engine.AttachScript(MakeEntity(15), "Broken", "ENG200Broken"));
}

TEST(ScriptLifecycle_ENG200_ConstructorExceptionReportsLine)
{
    ScriptFaultFixture fx;
    EXPECT_TRUE(fx.ready);

    const char* const badCtor = "class BadCtor\n"             // 1
                                "{\n"                         // 2
                                "    int zero = 0;\n"         // 3
                                "    int value;\n"            // 4
                                "    BadCtor()\n"             // 5
                                "    {\n"                     // 6
                                "        value = 1 / zero;\n" // 7
                                "    }\n"                     // 8
                                "}\n";
    EXPECT_TRUE(fx.engine.CompileScriptFromString(badCtor, "ENG200Ctor"));
    const EntityID entity = MakeEntity(16);
    EXPECT_FALSE(fx.engine.AttachScript(entity, "BadCtor", "ENG200Ctor"));

    const std::string error = fx.engine.GetLastError();
    EXPECT_STR_CONTAINS(error, "BadCtor");
    EXPECT_STR_CONTAINS(error, "Divide by zero");
    EXPECT_STR_CONTAINS(error, "ENG200Ctor:7");
    EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
