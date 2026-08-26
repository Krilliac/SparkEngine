/**
 * @file TestCoverageScripting.cpp
 * @brief Production coverage for script hot reload, sandbox diagnostics, and visual compilation.
 */

#include "TestFramework.h"

#include "Engine/Scripting/ScriptHotReload.h"
#include "Engine/Scripting/ScriptSandbox.h"
#include "Engine/Scripting/VisualScriptCompiler.h"
#include "Utils/SparkConsole.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace Spark::Scripting;

    class TempScriptTree
    {
      public:
        explicit TempScriptTree(const char* tag)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = fs::temp_directory_path() /
                     (std::string("spark-coverage-scripting-") + tag + "-" + std::to_string(stamp));
            fs::create_directories(m_root);
        }

        ~TempScriptTree()
        {
            std::error_code ec;
            fs::remove_all(m_root, ec);
        }

        const fs::path& Root() const { return m_root; }

        fs::path Write(const fs::path& relative, const std::string& contents) const
        {
            const fs::path path = m_root / relative;
            fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << contents;
            output.close();
            return path;
        }

      private:
        fs::path m_root;
    };

    ScriptPin Pin(PinKind kind, float x = 0.0f, float y = 0.0f, float z = 0.0f, std::string text = {})
    {
        ScriptPin pin;
        pin.kind = kind;
        pin.defaultValue[0] = x;
        pin.defaultValue[1] = y;
        pin.defaultValue[2] = z;
        pin.defaultString = std::move(text);
        return pin;
    }

    ScriptCompileResult CompileSingleNode(ScriptNode node, bool debugMode = false)
    {
        ScriptNode start;
        start.id = 1;
        start.type = ScriptNodeType::OnStart;
        start.outputs = {Pin(PinKind::Execution)};

        node.id = 2;
        VisualScriptGraph graph;
        graph.className = "CoverageScript";
        graph.nodes = {std::move(start), std::move(node)};
        // Route execution to the node without occupying one of its data inputs.
        // Hand-authored graphs are allowed to omit pin metadata, and the compiler
        // deliberately treats the known event output as an execution connection.
        graph.connections = {{1, 0, 2, 999}};
        return VisualScriptCompiler::Compile(graph, debugMode);
    }
} // namespace

TEST(CoverageScripting_HotReloadTracksRealFilesAndProcessesChanges)
{
    TempScriptTree scripts("success");
    const auto mainScript = scripts.Write("main.as", "void main() {}\n");
    scripts.Write("nested/child.angelscript", "void child() {}\n");
    scripts.Write("ignored.txt", "not a script\n");

    int callbackCount = 0;
    Spark::Scripting::ScriptHotReloadManager manager;
    manager.SetDebounceMs(0);
    manager.SetRecompileCallback(
        [&](const std::string& file)
        {
            ++callbackCount;
            RecompileResult result;
            result.success = true;
            result.filePath = file;
            return result;
        });
    manager.AddWatchDirectory(scripts.Root().string());
    manager.Start();
    manager.Start(); // Starting an already-running watcher is intentionally idempotent.

    EXPECT_TRUE(manager.IsRunning());
    EXPECT_EQ(manager.GetWatchedFileCount(), 2);
    EXPECT_EQ(manager.PollChanges(), 0);
    EXPECT_EQ(manager.RecompileAll(), 2);
    EXPECT_EQ(callbackCount, 2);
    EXPECT_EQ(manager.GetRecompileCount(), 2);
    EXPECT_EQ(manager.GetErrorCount(), 0);

    {
        std::ofstream append(mainScript, std::ios::binary | std::ios::app);
        append << "// changed and longer\n";
    }
    EXPECT_EQ(manager.PollChanges(), 1);
    EXPECT_EQ(callbackCount, 3);

    // Files created after Start are discovered without being treated as modifications.
    scripts.Write("created.as", "void created() {}\n");
    EXPECT_EQ(manager.PollChanges(), 0);
    EXPECT_EQ(manager.GetWatchedFileCount(), 3);

    TempScriptTree additional("nonrecursive");
    additional.Write("direct.as", "void direct() {}\n");
    additional.Write("nested/deep.as", "void deep() {}\n");
    manager.AddWatchDirectory(additional.Root().string(), false);
    EXPECT_EQ(manager.GetWatchedFileCount(), 4);
    // Regular polling uses the manager's recursive watch policy and finds the nested file.
    EXPECT_EQ(manager.PollChanges(), 0);
    EXPECT_EQ(manager.GetWatchedFileCount(), 5);

    EXPECT_STR_CONTAINS(manager.Console_GetStatus(), "Running");
    EXPECT_STR_CONTAINS(manager.Console_GetStatus(), "Recompiles: 3");
    manager.Stop();
    manager.Stop();
    EXPECT_FALSE(manager.IsRunning());
    EXPECT_EQ(manager.PollChanges(), 0);
    EXPECT_STR_CONTAINS(manager.Console_GetStatus(), "Stopped");
}

TEST(CoverageScripting_HotReloadBoundsErrorHistoryAndReportsFailures)
{
    TempScriptTree scripts("errors");
    for (int i = 0; i < 12; ++i)
    {
        scripts.Write("broken" + std::to_string(i) + ".as", "syntax error\n");
    }

    int errorCallbacks = 0;
    Spark::Scripting::ScriptHotReloadManager manager;
    manager.SetWatchExtensions({".as"});
    manager.SetRecompileCallback(
        [](const std::string& file)
        {
            RecompileResult result;
            result.success = false;
            result.filePath = file;
            result.errorLine = 7;
            result.errorMessage = "expected ';'";
            return result;
        });
    manager.SetErrorCallback(
        [&](const RecompileResult& result)
        {
            ++errorCallbacks;
            EXPECT_EQ(result.errorLine, 7);
        });
    manager.AddWatchDirectory(scripts.Root().string());
    manager.Start();

    EXPECT_EQ(manager.RecompileAll(), 12);
    EXPECT_EQ(manager.GetRecompileCount(), 12);
    EXPECT_EQ(manager.GetErrorCount(), 12);
    EXPECT_EQ(errorCallbacks, 12);
    EXPECT_EQ(manager.GetRecentErrors().size(), static_cast<size_t>(10));
    EXPECT_STR_CONTAINS(manager.Console_GetStatus(), "Errors: 12");
    EXPECT_STR_CONTAINS(manager.Console_GetStatus(), "expected ';'");

    manager.Stop();

    // A watcher without a compiler callback still scans safely and reports no compile.
    Spark::Scripting::ScriptHotReloadManager noCompiler;
    noCompiler.AddWatchDirectory(scripts.Root().string());
    noCompiler.Start();
    EXPECT_EQ(noCompiler.RecompileAll(), 12);
    EXPECT_EQ(noCompiler.GetRecompileCount(), 0);
}

TEST(CoverageScripting_SandboxConsoleAndNestedExecutionPaths)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    EXPECT_TRUE(console.Initialize());

    Spark::ScriptSandbox sandbox;
    sandbox.SetInstructionLimit(42);
    sandbox.SetExecutionTimeout(0.25f);
    EXPECT_STR_CONTAINS(sandbox.GetStatusString(), "Instructions: 42");
    EXPECT_STR_CONTAINS(sandbox.GetStatusString(), "Timeout: 0.250s");

    sandbox.BeginExecution("outer");
    sandbox.BeginExecution("inner");
    sandbox.EndExecution();
    EXPECT_FALSE(sandbox.WasTerminated());
    sandbox.EndExecution();
    sandbox.EndExecution(); // Unbalanced cleanup is a safe no-op.

    sandbox.RegisterConsoleCommands();
    EXPECT_TRUE(console.HasCommand("sandbox.status"));
    EXPECT_TRUE(console.HasCommand("sandbox.level"));
    EXPECT_TRUE(console.HasCommand("sandbox.violations"));
    EXPECT_TRUE(console.ExecuteCommand("sandbox.status"));
    EXPECT_TRUE(console.ExecuteCommand("sandbox.level"));
    EXPECT_TRUE(console.ExecuteCommand("sandbox.level unrestricted"));
    EXPECT_TRUE(sandbox.GetSecurityLevel() == Spark::ScriptSecurityLevel::Unrestricted);
    EXPECT_TRUE(console.ExecuteCommand("sandbox.level standard"));
    EXPECT_TRUE(console.ExecuteCommand("sandbox.level strict"));
    EXPECT_TRUE(console.ExecuteCommand("sandbox.level impossible"));
    EXPECT_TRUE(console.ExecuteCommand("sandbox.violations"));

    sandbox.UnregisterConsoleCommands();
    sandbox.UnregisterConsoleCommands();
    EXPECT_FALSE(console.HasCommand("sandbox.status"));
}

TEST(CoverageScripting_VisualCompilerEmitsAllDataAndActionFamilies)
{
    struct Case
    {
        ScriptNode node;
        const char* expected;
    };

    const auto floatInputs =
        std::vector<ScriptPin>{Pin(PinKind::Float, 2.0f), Pin(PinKind::Float, 3.0f), Pin(PinKind::Float, 0.5f)};
    const auto floatOutput = std::vector<ScriptPin>{Pin(PinKind::Float, 4.0f)};
    const auto boolInputs = std::vector<ScriptPin>{Pin(PinKind::Bool, 1.0f), Pin(PinKind::Bool, 0.0f)};
    const auto boolOutput = std::vector<ScriptPin>{Pin(PinKind::Bool)};
    const auto vectorInputs = std::vector<ScriptPin>{Pin(PinKind::Vector3, 1, 2, 3), Pin(PinKind::Vector3, 4, 5, 6)};
    const auto vectorActionInputs =
        std::vector<ScriptPin>{Pin(PinKind::Execution), Pin(PinKind::Entity, 9), Pin(PinKind::Vector3, 1, 2, 3)};
    const auto floatActionInputs =
        std::vector<ScriptPin>{Pin(PinKind::Execution), Pin(PinKind::Entity, 9), Pin(PinKind::Float, 50)};

    std::vector<Case> cases = {
        {{0, ScriptNodeType::ConstInt, {}, {Pin(PinKind::Int, 7)}}, "int n2_out0 = 7"},
        {{0, ScriptNodeType::ConstBool, {}, {Pin(PinKind::Bool, 1)}}, "bool n2_out0 = true"},
        {{0, ScriptNodeType::ConstString, {}, {Pin(PinKind::String, 0, 0, 0, "line\\n\"")}}, "string n2_out0"},
        {{0, ScriptNodeType::ConstVector3, {}, {Pin(PinKind::Vector3, 1, 2, 3)}}, "Vector3 n2_out0"},
        {{0, ScriptNodeType::Subtract, floatInputs, floatOutput}, "2.000000f - 3.000000f"},
        {{0, ScriptNodeType::Divide, floatInputs, floatOutput}, "!= 0.0f"},
        {{0, ScriptNodeType::Negate, floatInputs, floatOutput}, "= -2.000000f"},
        {{0, ScriptNodeType::Abs, floatInputs, floatOutput}, "abs(2.000000f)"},
        {{0, ScriptNodeType::Lerp, floatInputs, floatOutput}, "- 2.000000f) * 0.500000f"},
        {{0, ScriptNodeType::Clamp, floatInputs, floatOutput}, "float _v2"},
        {{0, ScriptNodeType::Random, {}, floatOutput}, "float(rand())"},
        {{0, ScriptNodeType::RandomRange, floatInputs, floatOutput}, "float(rand())"},
        {{0, ScriptNodeType::And, boolInputs, boolOutput}, "&&"},
        {{0, ScriptNodeType::Or, boolInputs, boolOutput}, "||"},
        {{0, ScriptNodeType::Not, boolInputs, boolOutput}, "!true"},
        {{0, ScriptNodeType::Equal, floatInputs, boolOutput}, "=="},
        {{0, ScriptNodeType::NotEqual, floatInputs, boolOutput}, "!="},
        {{0, ScriptNodeType::Greater, floatInputs, boolOutput}, ">"},
        {{0, ScriptNodeType::Less, floatInputs, boolOutput}, "<"},
        {{0, ScriptNodeType::GreaterEqual, floatInputs, boolOutput}, ">="},
        {{0, ScriptNodeType::LessEqual, floatInputs, boolOutput}, "<="},
        {{0, ScriptNodeType::GetKeyDown, {}, boolOutput, {{"key", "Enter"}}}, "getKeyDown(\"Enter\")"},
        {{0, ScriptNodeType::GetKey, {}, boolOutput}, "getKey(\"Space\")"},
        {{0, ScriptNodeType::GetDeltaTime, {}, floatOutput}, "= dt"},
        {{0, ScriptNodeType::GetSelf, {}, {Pin(PinKind::Entity)}}, "= selfEntity"},
        {{0, ScriptNodeType::GetPosition, {Pin(PinKind::Entity, 9)}, {Pin(PinKind::Vector3)}}, "getPosition(9)"},
        {{0, ScriptNodeType::GetRotation, {Pin(PinKind::Entity, 9)}, {Pin(PinKind::Vector3)}}, "getRotation(9)"},
        {{0, ScriptNodeType::GetHealth, {Pin(PinKind::Entity, 9)}, floatOutput}, "getHealth(9)"},
        {{0, ScriptNodeType::GetSpeed, {Pin(PinKind::Entity, 9)}, floatOutput}, "getSpeed(9)"},
        {{0, ScriptNodeType::GetEntityByName, {}, {Pin(PinKind::Entity)}, {{"name", "Boss"}}},
         "getEntityByName(\"Boss\")"},
        {{0, ScriptNodeType::SetPosition, vectorActionInputs, {}}, "setPosition("},
        {{0, ScriptNodeType::SetRotation, vectorActionInputs, {}}, "setRotation("},
        {{0, ScriptNodeType::SetHealth, floatActionInputs, {}}, "setHealth("},
        {{0, ScriptNodeType::ApplyForce, vectorActionInputs, {}}, "applyForce("},
        {{0, ScriptNodeType::PlaySound, {Pin(PinKind::Execution)}, {}, {{"sound", "boom.wav"}}},
         "playSound(selfEntity, \"boom.wav\")"},
        {{0, ScriptNodeType::PlayAnimation, {Pin(PinKind::Execution)}, {}, {{"animation", "run"}}},
         "playAnimation(selfEntity, \"run\")"},
        {{0, ScriptNodeType::SpawnEntity, {Pin(PinKind::Execution)}, {Pin(PinKind::Entity)}, {{"name", "Enemy"}}},
         "createEntity(\"Enemy\")"},
        {{0, ScriptNodeType::DestroyEntity, {Pin(PinKind::Execution), Pin(PinKind::Entity, 8)}, {}},
         "destroyEntity(8)"},
        {{0, ScriptNodeType::PrintMessage, {Pin(PinKind::Execution), Pin(PinKind::String, 0, 0, 0, "hello")}, {}},
         "print(\"hello\")"},
        {{0, ScriptNodeType::FireEvent, {Pin(PinKind::Execution)}, {}, {{"event", "Won"}}}, "fireEvent(\"Won\")"},
        {{0,
          ScriptNodeType::ForLoop,
          {Pin(PinKind::Execution), Pin(PinKind::Int, 1), Pin(PinKind::Int, 3)},
          {Pin(PinKind::Execution), Pin(PinKind::Int)}},
         "for (int n2_out1 = 1"},
        {{0, ScriptNodeType::GetVariable, {}, {Pin(PinKind::Bool)}, {{"name", "is ready"}}}, "= is_ready"},
        {{0, ScriptNodeType::SetVariable, {Pin(PinKind::Execution), Pin(PinKind::Float, 5)}, {}, {{"name", "score"}}},
         "score = 5.000000f"},
        {{0, ScriptNodeType::CallFunction, {Pin(PinKind::Float, 2)}, {Pin(PinKind::Float)}, {{"function", "Twice"}}},
         "Twice(2.000000f)"},
        {{0, ScriptNodeType::ReturnValue, {Pin(PinKind::Float, 9)}, {}}, "return 9.000000f"},
        {{0, ScriptNodeType::Normalize, vectorInputs, {Pin(PinKind::Vector3)}}, "normalize("},
        {{0, ScriptNodeType::DotProduct, vectorInputs, floatOutput}, "dot("},
        {{0, ScriptNodeType::Distance, vectorInputs, floatOutput}, "distance("},
        {{0, ScriptNodeType::Comment, {}, {}}, "Unhandled node type 500"},
    };

    for (auto& testCase : cases)
    {
        const auto result = CompileSingleNode(std::move(testCase.node), true);
        EXPECT_TRUE(result.success);
        EXPECT_STR_CONTAINS(result.angelScriptSource, testCase.expected);
        EXPECT_STR_CONTAINS(result.angelScriptSource, "debugTrace(2");
    }
}

TEST(CoverageScripting_VisualCompilerEventsFunctionsMetadataAndFallbacks)
{
    using namespace Spark::Scripting;
    const auto& palette = VisualScriptCompiler::GetNodePalette();
    EXPECT_GT(palette.size(), static_cast<size_t>(50));
    EXPECT_EQ(std::string(VisualScriptCompiler::GetNodeDisplayName(ScriptNodeType::OnStart)), std::string("On Start"));
    EXPECT_EQ(std::string(VisualScriptCompiler::GetNodeCategory(ScriptNodeType::PlaySound)), std::string("Actions"));
    EXPECT_EQ(std::string(VisualScriptCompiler::GetNodeDisplayName(static_cast<ScriptNodeType>(9999))),
              std::string("Unknown"));
    EXPECT_EQ(std::string(VisualScriptCompiler::GetNodeCategory(static_cast<ScriptNodeType>(9999))),
              std::string("Misc"));

    VisualScriptGraph graph;
    graph.className = "9 invalid class";
    const std::vector<ScriptNodeType> eventTypes = {ScriptNodeType::OnStart,        ScriptNodeType::OnUpdate,
                                                    ScriptNodeType::OnTriggerEnter, ScriptNodeType::OnTriggerExit,
                                                    ScriptNodeType::OnDamaged,      ScriptNodeType::OnKeyPress,
                                                    ScriptNodeType::OnCollision,    ScriptNodeType::OnCustomEvent};
    uint32_t id = 1;
    for (const auto type : eventTypes)
    {
        ScriptNode event;
        event.id = id++;
        event.type = type;
        event.outputs = {Pin(PinKind::Execution)};
        if (type == ScriptNodeType::OnKeyPress)
            event.properties["key"] = "K";
        graph.nodes.push_back(std::move(event));
    }
    graph.variables.push_back({"player score", PinKind::Int, "7"});

    FunctionGraph function;
    function.name = "double value";
    function.returnType = PinKind::Float;
    function.parameters.push_back({"input value", PinKind::Float, {}});
    function.nodes.push_back({100, ScriptNodeType::ReturnValue, {Pin(PinKind::Float, 2)}, {}, {}});
    graph.functions.push_back(std::move(function));
    graph.customEvents.push_back({"boss defeated", {{"reward points", PinKind::Int, {}}}});

    const auto result = VisualScriptCompiler::Compile(graph);
    EXPECT_TRUE(result.success);
    EXPECT_STR_CONTAINS(result.angelScriptSource, "class _9_invalid_class");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "void Start()");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "void Update(float dt)");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "void OnCollision(uint other)");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "void OnTriggerEnter(uint triggerId)");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "void OnTriggerExit(uint triggerId)");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "void OnDamaged(float amount)");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "if (getKeyDown(\"K\"))");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "int player_score = 7");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "float double_value(float input_value)");
    EXPECT_STR_CONTAINS(result.angelScriptSource, "void Onboss_defeated(int reward_points)");
}
