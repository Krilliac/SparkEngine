/**
 * @file TestSEC3ScriptingHardening.cpp
 * @brief SEC3: scripting trust-boundary regressions (visual-script compiler, script API, hot reload, sandbox).
 *
 * SEC3VisualScript_* (no AngelScript SDK needed):
 * - A crafted graph of deeply nested Branch nodes, or a long chain of pure data
 *   nodes, used to recurse through VisualScriptEmitter until the stack
 *   overflowed. The emitter now fails such graphs with a compile error.
 * - Editor script names become file names; VisualScriptGraphIO::ScriptFilePath
 *   refuses anything that is not a plain identifier or is a device name.
 *
 * SEC3Script_* (real AngelScriptEngine):
 * - A `Transform@` kept past destroyEntity() used to be a raw pointer into the
 *   EnTT pool, so writes through it landed on whichever entity was swapped into
 *   the slot. The handle now re-resolves its entity and faults the script.
 * - getPosition()/setHealth()/getComponentField()... on a destroyed entity id
 *   used to hit World's fatal "invalid entity" precondition.
 * - HotReloadModule() re-read the source file after detaching live instances;
 *   a file rewritten in between left every entity without a script.
 * - `sandbox.level strict` after Initialize() reported Strict while the
 *   Standard API stayed callable; the function policy is now fixed at startup.
 */

#include "TestFramework.h"

#include "Engine/Scripting/VisualScriptCompiler.h"
#include "Engine/Scripting/VisualScriptGraphIO.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    using namespace Spark::Scripting;

    ScriptPin Pin(PinKind kind)
    {
        ScriptPin pin;
        pin.kind = kind;
        return pin;
    }

    /// OnStart -> Branch(2) -True-> Branch(3) -True-> ... : @p depth Branch nodes, each inside the previous one.
    VisualScriptGraph NestedBranchGraph(uint32_t depth)
    {
        VisualScriptGraph graph;
        graph.className = "Nested";
        ScriptNode start;
        start.id = 1;
        start.type = ScriptNodeType::OnStart;
        graph.nodes.push_back(start);
        for (uint32_t i = 0; i < depth; ++i)
        {
            ScriptNode branch;
            branch.id = i + 2;
            branch.type = ScriptNodeType::Branch;
            branch.inputs = {Pin(PinKind::Execution), Pin(PinKind::Bool)};
            branch.outputs = {Pin(PinKind::Execution), Pin(PinKind::Execution)};
            graph.nodes.push_back(branch);
            graph.connections.push_back({i + 1, 0, i + 2, 0});
        }
        return graph;
    }

    bool HasErrorContaining(const ScriptCompileResult& result, const std::string& text)
    {
        for (const auto& error : result.errors)
        {
            if (error.find(text) != std::string::npos)
                return true;
        }
        return false;
    }
} // namespace

TEST(SEC3VisualScript_NestedBranchChainFailsCleanly)
{
    // The parser admits 16,384 nodes. Before the depth budget each nesting level
    // cost four native frames plus a growing indent string, so this graph
    // overflowed the stack (or compiled to hundreds of MB of source).
    const auto result = VisualScriptCompiler::Compile(NestedBranchGraph(16383));
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.angelScriptSource.empty());
    EXPECT_TRUE(HasErrorContaining(result, "nests execution flow deeper than"));
}

TEST(SEC3VisualScript_ModerateNestingStillCompiles)
{
    const auto result = VisualScriptCompiler::Compile(NestedBranchGraph(32));
    EXPECT_TRUE(result.success);
    // All 32 levels are emitted.
    size_t ifs = 0;
    for (size_t at = result.angelScriptSource.find("if ("); at != std::string::npos;
         at = result.angelScriptSource.find("if (", at + 1))
    {
        ++ifs;
    }
    EXPECT_EQ(ifs, static_cast<size_t>(32));
}

TEST(SEC3VisualScript_DeepPureDataChainFailsCleanly)
{
    // SetVariable <- Add(1000) <- Add(1001) <- ... : one pure producer per level,
    // which CollectPure walked by recursion with no bound.
    constexpr uint32_t kChain = 2000;
    VisualScriptGraph graph;
    graph.className = "DataChain";
    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    ScriptNode setVar;
    setVar.id = 2;
    setVar.type = ScriptNodeType::SetVariable;
    setVar.properties["name"] = "total";
    setVar.inputs = {Pin(PinKind::Execution), Pin(PinKind::Float)};
    graph.nodes.push_back(setVar);
    graph.connections.push_back({1, 0, 2, 0});

    for (uint32_t i = 0; i < kChain; ++i)
    {
        ScriptNode add;
        add.id = 1000 + i;
        add.type = ScriptNodeType::Add;
        add.inputs = {Pin(PinKind::Float), Pin(PinKind::Float)};
        add.outputs = {Pin(PinKind::Float)};
        graph.nodes.push_back(add);
        // Each Add feeds input 0 of the previous one; the first feeds the SetVariable value.
        graph.connections.push_back({add.id, 0, i == 0 ? 2u : add.id - 1, i == 0 ? 1u : 0u});
    }

    const auto result = VisualScriptCompiler::Compile(graph);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(HasErrorContaining(result, "Data chain into node"));
}

TEST(SEC3VisualScript_ScriptFilePathRejectsPathsAndDeviceNames)
{
    const std::filesystem::path directory = "Assets/Scripts/Generated";

    const auto good = VisualScriptGraphIO::ScriptFilePath(directory, "Player_Health2", ".as");
    EXPECT_TRUE(good.has_value());
    if (good)
    {
        EXPECT_TRUE(good->parent_path() == directory);
        EXPECT_TRUE(good->filename() == std::filesystem::path("Player_Health2.as"));
    }

    const char* const refused[] = {
        "",
        "../../../evil",
        "..\\..\\evil",
        "sub/dir",
        "/abs/path",
        "C:evil",
        "C:\\Windows\\evil",
        "a.b",
        "..",
        "has space",
        "9lives",
        "NUL",
        "con",
        "Com1",
        "lpt9",
    };
    for (const char* name : refused)
    {
        const auto path = VisualScriptGraphIO::ScriptFilePath(directory, name, ".as");
        EXPECT_FALSE(path.has_value());
        if (path)
            std::printf("  accepted unsafe script name '%s' -> %s\n", name, path->string().c_str());
    }

    // Over-long names are refused rather than truncated into a different file.
    EXPECT_FALSE(VisualScriptGraphIO::ScriptFilePath(directory, std::string(257, 'a'), ".vscript").has_value());
    EXPECT_TRUE(VisualScriptGraphIO::ScriptFilePath(directory, std::string(256, 'a'), ".vscript").has_value());
}

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "Engine/Scripting/AngelScriptEngine.h"
#include "Utils/SparkConsole.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>

namespace
{
    std::string g_sec3Trace; ///< Output of the last debugTrace(1, ...) call

    void CaptureSec3Trace(uint32_t nodeId, const char* /*nodeName*/, const char* output)
    {
        if (nodeId == 1)
            g_sec3Trace = output ? output : "";
    }

    /// A real, initialized AngelScriptEngine bound to a fresh World.
    struct Sec3ScriptFixture
    {
        AngelScriptEngine engine;
        World world;
        bool ready = false;

        Sec3ScriptFixture()
        {
            g_sec3Trace.clear();
            ASSetDebugTraceCallback(&CaptureSec3Trace);
            ready = engine.Initialize();
            AngelScriptEngine::BindWorld(&world);
        }

        ~Sec3ScriptFixture()
        {
            AngelScriptEngine::BindWorld(nullptr);
            engine.Shutdown();
            ASSetDebugTraceCallback(nullptr);
        }

        Sec3ScriptFixture(const Sec3ScriptFixture&) = delete;
        Sec3ScriptFixture& operator=(const Sec3ScriptFixture&) = delete;

        bool Compile(const char* source, const char* moduleName)
        {
            const bool compiled = engine.CompileScriptFromString(source, moduleName);
            if (!compiled)
                std::printf("  compile diagnostic: %s\n", engine.GetLastError().c_str());
            return compiled;
        }
    };

    bool NearVector(const DirectX::XMFLOAT3& v, float x, float y, float z)
    {
        return std::fabs(v.x - x) < 1e-5f && std::fabs(v.y - y) < 1e-5f && std::fabs(v.z - z) < 1e-5f;
    }
} // namespace

TEST(SEC3Script_RetainedTransformHandleCannotReachAnotherEntity)
{
    Sec3ScriptFixture fx;
    EXPECT_TRUE(fx.ready);

    // A and B share the Transform pool; destroying A swaps B into A's slot, which
    // is where a raw pointer kept from getTransform(A) pointed.
    const EntityID a = fx.world.CreateEntity("A");
    fx.world.AddComponent<Transform>(a);
    const EntityID b = fx.world.CreateEntity("B");
    fx.world.AddComponent<Transform>(b).position = {1.0f, 2.0f, 3.0f};
    const EntityID host = fx.world.CreateEntity("Host");

    const char* const script = "class Keeper\n"
                               "{\n"
                               "    Transform@ kept;\n"
                               "    void Start()\n"
                               "    {\n"
                               "        EntityID a = getEntityByName(\"A\");\n"
                               "        @kept = getTransform(a);\n"
                               "        destroyEntity(a);\n"
                               "    }\n"
                               "    void Update(float dt)\n"
                               "    {\n"
                               "        kept.position = Vector3(42.0f, 42.0f, 42.0f);\n"
                               "    }\n"
                               "}\n";
    EXPECT_TRUE(fx.Compile(script, "SEC3Keeper"));
    EXPECT_TRUE(fx.engine.AttachScript(host, "Keeper", "SEC3Keeper"));

    fx.engine.CallStart(host);
    EXPECT_FALSE(fx.world.GetRegistry().valid(a));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(host));

    fx.engine.CallUpdate(host, 0.016f);

    // B is untouched, and the stale write surfaced as a script fault instead.
    EXPECT_TRUE(NearVector(fx.world.GetComponent<Transform>(b)->position, 1.0f, 2.0f, 3.0f));
    EXPECT_TRUE(fx.engine.IsScriptFaulted(host));
    EXPECT_STR_CONTAINS(fx.engine.GetLastError(), "Transform handle used after its entity was destroyed");

    fx.engine.DetachScript(host);
}

TEST(SEC3Script_LiveTransformHandleReadsAndWrites)
{
    Sec3ScriptFixture fx;
    EXPECT_TRUE(fx.ready);

    const EntityID mover = fx.world.CreateEntity("Mover");
    fx.world.AddComponent<Transform>(mover);
    const EntityID bare = fx.world.CreateEntity("Bare");
    (void)bare;

    const char* const script =
        "class Probe\n"
        "{\n"
        "    void Start()\n"
        "    {\n"
        "        Transform@ t = getTransform(getEntityByName(\"Mover\"));\n"
        "        t.position = Vector3(5.0f, 6.0f, 7.0f);\n"
        "        t.rotation = Vector3(0.5f, 0.0f, 0.0f);\n"
        "        t.scale = Vector3(2.0f, 2.0f, 2.0f);\n"
        "        Transform@ none = getTransform(getEntityByName(\"Bare\"));\n"
        "        debugTrace(1, \"t\", \"\" + int(t.position.y) + \"|\" + (t.isValid() ? 1 : 0) + \"|\" +\n"
        "                   (none is null ? 1 : 0) + \"|\" + t.entity);\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(fx.Compile(script, "SEC3Probe"));
    EXPECT_TRUE(fx.engine.AttachScript(mover, "Probe", "SEC3Probe"));
    fx.engine.CallStart(mover);

    EXPECT_FALSE(fx.engine.IsScriptFaulted(mover));
    const Transform& transform = *fx.world.GetComponent<Transform>(mover);
    EXPECT_TRUE(NearVector(transform.position, 5.0f, 6.0f, 7.0f));
    EXPECT_TRUE(NearVector(transform.rotation, 0.5f, 0.0f, 0.0f));
    EXPECT_TRUE(NearVector(transform.scale, 2.0f, 2.0f, 2.0f));
    EXPECT_EQ(g_sec3Trace, "6|1|1|" + std::to_string(static_cast<uint32_t>(mover)));

    fx.engine.DetachScript(mover);
}

TEST(SEC3Script_StaleEntityIdAccessorsAreSafe)
{
    Sec3ScriptFixture fx;
    EXPECT_TRUE(fx.ready);

    const EntityID doomed = fx.world.CreateEntity("Doomed");
    fx.world.AddComponent<Transform>(doomed).position = {9.0f, 9.0f, 9.0f};
    HealthComponent health;
    health.health = 50.0f;
    fx.world.AddComponent<HealthComponent>(doomed, health);
    const EntityID host = fx.world.CreateEntity("Host");

    // Every entity-id accessor on a destroyed id used to reach World::HasComponent /
    // GetComponent, whose "invalid entity" precondition is a fatal assertion.
    const char* const script = "class Stale\n"
                               "{\n"
                               "    void Start()\n"
                               "    {\n"
                               "        EntityID d = getEntityByName(\"Doomed\");\n"
                               "        destroyEntity(d);\n"
                               "        destroyEntity(d);\n"
                               "        Vector3 p = getPosition(d);\n"
                               "        setPosition(d, Vector3(1.0f, 1.0f, 1.0f));\n"
                               "        Vector3 r = getRotation(d);\n"
                               "        setRotation(d, r);\n"
                               "        float h = getHealth(d);\n"
                               "        setHealth(d, 5.0f);\n"
                               "        bool has = hasComponent(d, \"Transform\");\n"
                               "        string f = getComponentField(d, \"Transform\", \"position\");\n"
                               "        setComponentField(d, \"Transform\", \"position\", \"1,1,1\");\n"
                               "        debugTrace(1, \"s\", \"\" + int(p.x) + \"|\" + int(h) + \"|\" + (has ? 1 : 0) "
                               "+ \"|\" + f + \"|\" + int(getSpeed(d)));\n"
                               "    }\n"
                               "}\n";
    EXPECT_TRUE(fx.Compile(script, "SEC3Stale"));
    EXPECT_TRUE(fx.engine.AttachScript(host, "Stale", "SEC3Stale"));
    fx.engine.CallStart(host);

    EXPECT_FALSE(fx.engine.IsScriptFaulted(host));
    EXPECT_FALSE(fx.world.GetRegistry().valid(doomed));
    EXPECT_EQ(g_sec3Trace, std::string("0|0|0||0"));

    fx.engine.DetachScript(host);
}

namespace
{
    std::string g_reloadState;       ///< Output of the last debugTrace(1, ...) from the reload probe
    std::string g_reloadPath;        ///< Script file the destructor hook rewrites
    bool g_rewriteOnDestroy = false; ///< Armed only around the HotReloadModule call
    int g_rewrites = 0;              ///< Times the hook rewrote the file

    /// debugTrace(99) comes from ~Probe(): the old instance is being released by the reload.
    void ReloadTrace(uint32_t nodeId, const char* /*nodeName*/, const char* output)
    {
        if (nodeId == 1)
        {
            g_reloadState = output ? output : "";
        }
        else if (nodeId == 99 && g_rewriteOnDestroy)
        {
            // An editor mid-save: the file no longer compiles.
            std::ofstream out(g_reloadPath, std::ios::binary | std::ios::trunc);
            out << "class Probe {";
            ++g_rewrites;
        }
    }

    const char* ReloadSource(const char* version)
    {
        return version[1] == '1' ? "class Probe\n"
                                   "{\n"
                                   "    int counter = 0;\n"
                                   "    ~Probe() { debugTrace(99, \"dtor\", \"\"); }\n"
                                   "    void Update(float dt) { counter++; debugTrace(1, \"s\", \"v1:\" + counter); }\n"
                                   "}\n"
                                 : "class Probe\n"
                                   "{\n"
                                   "    int counter = 0;\n"
                                   "    ~Probe() { debugTrace(99, \"dtor\", \"\"); }\n"
                                   "    void Update(float dt) { counter++; debugTrace(1, \"s\", \"v2:\" + counter); }\n"
                                   "}\n";
    }

    void WriteFile(const std::string& path, const char* text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
} // namespace

TEST(SEC3Script_HotReloadSurvivesSourceRewrittenDuringCommit)
{
    namespace fs = std::filesystem;
    static std::atomic<uint32_t> sequence{0};
    const fs::path directory =
        fs::temp_directory_path() /
        ("spark_sec3_reload_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
         std::to_string(sequence++));
    std::error_code error;
    fs::create_directories(directory, error);
    EXPECT_FALSE(static_cast<bool>(error));

    g_reloadState.clear();
    g_rewrites = 0;
    g_rewriteOnDestroy = false;
    g_reloadPath = (directory / "SEC3Reload.as").string();
    {
        AngelScriptEngine engine;
        ASSetDebugTraceCallback(&ReloadTrace);
        EXPECT_TRUE(engine.Initialize());

        WriteFile(g_reloadPath, ReloadSource("v1"));
        EXPECT_TRUE(engine.CompileScriptFile(g_reloadPath));
        const EntityID entity = static_cast<EntityID>(31);
        EXPECT_TRUE(engine.AttachScript(entity, "Probe", "SEC3Reload"));
        engine.CallUpdate(entity, 0.0f);
        engine.CallUpdate(entity, 0.0f);
        EXPECT_EQ(g_reloadState, std::string("v1:2"));

        // A valid v2 is on disk when the reload stages it. Releasing the old
        // instance during the commit then rewrites the file to invalid source:
        // the previous implementation compiled the file a second time at that
        // point, failed, and left the entity with no script.
        WriteFile(g_reloadPath, ReloadSource("v2"));
        g_rewriteOnDestroy = true;
        const bool reloaded = engine.HotReloadModule("SEC3Reload");
        g_rewriteOnDestroy = false;
        if (!reloaded)
            std::printf("  reload diagnostic: %s\n", engine.GetLastError().c_str());

        EXPECT_EQ(g_rewrites, 1); // the race window was actually exercised
        EXPECT_TRUE(reloaded);
        EXPECT_EQ(engine.GetEntitiesForModule("SEC3Reload").size(), static_cast<size_t>(1));
        engine.CallUpdate(entity, 0.0f);
        EXPECT_EQ(g_reloadState, std::string("v2:3")); // new code, carried counter

        // The broken file on disk now fails staging and leaves the live v2 script alone.
        EXPECT_FALSE(engine.HotReloadModule("SEC3Reload"));
        engine.CallUpdate(entity, 0.0f);
        EXPECT_EQ(g_reloadState, std::string("v2:4"));

        engine.DetachScript(entity);
        engine.Shutdown();
        ASSetDebugTraceCallback(nullptr);
    }
    fs::remove_all(directory, error);
}

TEST(SEC3Script_SandboxFunctionPolicyIsFixedAfterInitialize)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    (void)console.Initialize();

    Sec3ScriptFixture fx;
    EXPECT_TRUE(fx.ready);
    Spark::ScriptSandbox* sandbox = fx.engine.GetSandbox();
    ASSERT_TRUE(sandbox != nullptr);
    EXPECT_TRUE(sandbox->IsFunctionPolicyLocked());
    EXPECT_TRUE(sandbox->GetSecurityLevel() == Spark::ScriptSecurityLevel::Standard);

    // The API was bound under Standard, so getPosition stays callable. Reporting
    // Strict now would be false assurance: every path to a new level is refused.
    EXPECT_FALSE(sandbox->SetSecurityLevel(Spark::ScriptSecurityLevel::Strict));
    EXPECT_FALSE(sandbox->AddAllowedFunction("print"));
    EXPECT_FALSE(sandbox->AddBlockedFunction("getPosition"));
    EXPECT_TRUE(console.ExecuteCommand("sandbox.level strict"));
    fx.engine.ConfigureSandboxSecurity(Spark::ScriptSecurityLevel::Strict, {"print"}, {});
    EXPECT_TRUE(sandbox->GetSecurityLevel() == Spark::ScriptSecurityLevel::Standard);
    EXPECT_TRUE(sandbox->IsFunctionAllowed("getPosition"));
    EXPECT_STR_CONTAINS(sandbox->GetStatusString(), "Standard (API registered at startup, fixed)");
    // Re-asserting the level in force is allowed (it resets the resource limits).
    EXPECT_TRUE(sandbox->SetSecurityLevel(Spark::ScriptSecurityLevel::Standard));

    const char* const usesPosition = "class UsesPosition\n"
                                     "{\n"
                                     "    void Start() { Vector3 p = getPosition(getEntityByName(\"x\")); }\n"
                                     "}\n";
    EXPECT_TRUE(fx.Compile(usesPosition, "SEC3PolicyStandard"));

    // A Strict policy staged before Initialize() is what gets registered and locked.
    AngelScriptEngine strictEngine;
    strictEngine.ConfigureSandboxSecurity(Spark::ScriptSecurityLevel::Strict, {"print", "debugTrace"}, {});
    EXPECT_TRUE(strictEngine.Initialize());
    ASSERT_TRUE(strictEngine.GetSandbox() != nullptr);
    EXPECT_TRUE(strictEngine.GetSandbox()->GetSecurityLevel() == Spark::ScriptSecurityLevel::Strict);
    EXPECT_FALSE(strictEngine.CompileScriptFromString(usesPosition, "SEC3PolicyStrict"));
    EXPECT_FALSE(strictEngine.GetSandbox()->SetSecurityLevel(Spark::ScriptSecurityLevel::Unrestricted));
    EXPECT_TRUE(strictEngine.GetSandbox()->GetSecurityLevel() == Spark::ScriptSecurityLevel::Strict);
    strictEngine.Shutdown();
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
