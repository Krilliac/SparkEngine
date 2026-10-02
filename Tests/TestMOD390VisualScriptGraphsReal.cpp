/**
 * @file TestMOD390VisualScriptGraphsReal.cpp
 * @brief MOD-390: engine-owned .vscript graph I/O, and the checked-in module graphs regenerate the shipped scripts.
 *
 * - VisualScriptGraphs_CheckedInGraphsRegenerateShippedScripts loads every
 *   GameModules/SparkGameVisualScript/Assets/Graphs/<Class>.vscript through
 *   VisualScriptGraphIO, compiles it with VisualScriptCompiler and requires the
 *   result to equal the shipped Assets/Scripts/Generated/<Class>.as byte for
 *   byte, and each graph file to be in canonical form. VisualScriptGameplay_*
 *   plays those shipped scripts to the win, so the objective is graph-authored.
 *   To regenerate after editing a graph, run this test with
 *   SPARK_VSCRIPT_OUTPUT_DIR=<dir>: it writes <Class>.as and the canonical
 *   <Class>.vscript there for review and copying over the checked-in files.
 * - The parser rejects every malformed element (fail closed), SaveFile never
 *   writes a graph that would not load, and a full graph round-trips exactly.
 * - Compiled graphs run in the production AngelScriptEngine: data nodes are
 *   evaluated where they are used (a read after a write sees the write), and
 *   Branch/Sequence/ForLoop nest, in event methods and function sub-graphs alike.
 *   Vector3 literals compile, and a custom event's chain runs when the graph raises it.
 * - A custom event node must name a declared custom event, and a node whose canonical
 *   property is missing reads the same fallback property on every standard library.
 * - A hostile diamond-shaped execution graph fails the compile in bounded time.
 */

#include "TestFramework.h"

#include "../GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoRuntime.h"
#include "Engine/Scripting/VisualScriptCompiler.h"
#include "Engine/Scripting/VisualScriptGraphIO.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT
#include "Engine/ECS/Components.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "ScopedLoggerBaseline.h"
#include "Utils/Logger.h"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace Spark::Scripting;

    const fs::path kModuleAssets = fs::path(SPARK_TEST_SOURCE_DIR) / "GameModules/SparkGameVisualScript/Assets";

    std::string ReadBytes(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << stream.rdbuf();
        return bytes.str();
    }

    /// "line N: expected '...' got '...'" for the first differing line, or empty when equal.
    std::string FirstDifference(const std::string& expected, const std::string& actual)
    {
        std::istringstream a(expected);
        std::istringstream b(actual);
        std::string lineA;
        std::string lineB;
        for (size_t line = 1;; ++line)
        {
            const bool moreA = static_cast<bool>(std::getline(a, lineA));
            const bool moreB = static_cast<bool>(std::getline(b, lineB));
            if (!moreA && !moreB)
                return expected == actual ? "" : "trailing bytes differ";
            if (moreA != moreB || lineA != lineB)
                return "line " + std::to_string(line) + ": expected '" + (moreA ? lineA : "<eof>") + "' got '" +
                       (moreB ? lineB : "<eof>") + "'";
        }
    }

    ScriptPin Pin(PinKind kind, float value = 0.0f, std::string text = {})
    {
        ScriptPin pin;
        pin.kind = kind;
        pin.defaultValue[0] = value;
        pin.defaultString = std::move(text);
        return pin;
    }

    ScriptNode Node(uint32_t id, ScriptNodeType type, std::vector<ScriptPin> inputs, std::vector<ScriptPin> outputs,
                    std::unordered_map<std::string, std::string> properties = {})
    {
        ScriptNode node;
        node.id = id;
        node.type = type;
        node.inputs = std::move(inputs);
        node.outputs = std::move(outputs);
        node.properties = std::move(properties);
        return node;
    }

    /// A minimal valid .vscript document the rejection cases mutate one element at a time.
    std::string ValidDocument(const std::string& override = {})
    {
        const std::string nodes =
            R"([{"id": 1, "type": "OnStart", "outputs": [{"kind": "Execution"}]},)"
            R"( {"id": 2, "type": "PrintMessage", "inputs": [{"kind": "Execution"}, {"kind": "String", "default": "hi"}],)"
            R"( "outputs": [{"kind": "Execution"}]}])";
        return R"({"format": "spark.vscript", "version": 1, "className": "Probe", "variables": [],)"
               R"( "nodes": )" +
               nodes + R"(, "connections": [{"from": [1, 0], "to": [2, 0]}])" + override + "}";
    }

    bool SameGraph(const VisualScriptGraph& a, const VisualScriptGraph& b)
    {
        return VisualScriptGraphIO::Serialize(a) == VisualScriptGraphIO::Serialize(b);
    }

    ScriptPin Vector3Pin(float x, float y, float z)
    {
        ScriptPin pin = Pin(PinKind::Vector3, x);
        pin.defaultValue[1] = y;
        pin.defaultValue[2] = z;
        return pin;
    }

    size_t CountOccurrences(std::string_view text, std::string_view needle)
    {
        size_t hits = 0;
        for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + 1))
            ++hits;
        return hits;
    }
} // namespace

TEST(VisualScriptGraphs_CheckedInGraphsRegenerateShippedScripts)
{
    const char* outputDir = std::getenv("SPARK_VSCRIPT_OUTPUT_DIR");
    for (const auto& asset : Spark::VisualScriptDemo::ScriptManifest)
    {
        const std::string className(asset.className);
        const fs::path graphPath = kModuleAssets / "Graphs" / (className + ".vscript");
        const fs::path scriptPath = kModuleAssets / "Scripts/Generated" / fs::path(asset.fileName);

        auto graph = VisualScriptGraphIO::LoadFile(graphPath);
        if (!graph)
            std::printf("  %s\n", graph.error().c_str());
        ASSERT_TRUE(graph.has_value());
        EXPECT_EQ(graph->className, className);

        const auto compiled = VisualScriptCompiler::Compile(*graph);
        for (const auto& error : compiled.errors)
            std::printf("  %s: %s\n", className.c_str(), error.c_str());
        ASSERT_TRUE(compiled.success);

        const std::string canonical = VisualScriptGraphIO::Serialize(*graph);
        if (outputDir && *outputDir)
        {
            fs::create_directories(outputDir);
            std::ofstream(fs::path(outputDir) / (className + ".as"), std::ios::binary) << compiled.angelScriptSource;
            std::ofstream(fs::path(outputDir) / (className + ".vscript"), std::ios::binary) << canonical;
        }

        const std::string scriptDiff = FirstDifference(ReadBytes(scriptPath), compiled.angelScriptSource);
        if (!scriptDiff.empty())
            std::printf("  %s is stale against its graph: %s\n", scriptPath.generic_string().c_str(),
                        scriptDiff.c_str());
        EXPECT_TRUE(scriptDiff.empty());

        const std::string graphDiff = FirstDifference(ReadBytes(graphPath), canonical);
        if (!graphDiff.empty())
            std::printf("  %s is not canonical: %s\n", graphPath.generic_string().c_str(), graphDiff.c_str());
        EXPECT_TRUE(graphDiff.empty());

        // The module binds each instance by rewriting its one selfEntity placeholder.
        EXPECT_TRUE(Spark::VisualScriptDemo::BindSelfEntity(compiled.angelScriptSource, 7).has_value());
    }
}

TEST(VisualScriptGraphs_SerializeParseRoundTripsEveryField)
{
    VisualScriptGraph graph;
    graph.className = "RoundTrip";
    graph.description = "Line one\nLine two — UTF-8 \"quoted\"";
    graph.variables = {
        {"speed", PinKind::Float, "2.5f"}, {"alive", PinKind::Bool, "true"}, {"label", PinKind::String, "\"x\""}};

    ScriptNode start = Node(1, ScriptNodeType::OnStart, {}, {Pin(PinKind::Execution)});
    start.editorX = -12.5f;
    start.editorY = 0.7071068f;
    ScriptNode print = Node(2, ScriptNodeType::PrintMessage, {Pin(PinKind::Execution), Pin(PinKind::String, 0, "a\tb")},
                            {Pin(PinKind::Execution)});
    ScriptNode vector = Node(3, ScriptNodeType::ConstVector3, {}, {Pin(PinKind::Vector3)});
    vector.outputs[0].defaultValue[0] = 1.0f;
    vector.outputs[0].defaultValue[1] = -2.0f;
    vector.outputs[0].defaultValue[2] = 0.1f;
    ScriptNode sound = Node(4, ScriptNodeType::PlaySound, {Pin(PinKind::Execution)}, {Pin(PinKind::Execution)},
                            {{"sound", "boom"}, {"bus", "sfx"}});
    ScriptNode integer = Node(5, ScriptNodeType::ConstInt, {}, {Pin(PinKind::Int, -42.0f)});
    ScriptNode flag = Node(6, ScriptNodeType::ConstBool, {}, {Pin(PinKind::Bool, 1.0f)});
    ScriptNode entity = Node(7, ScriptNodeType::ConstInt, {}, {Pin(PinKind::Entity, 9.0f)});
    graph.nodes = {start, print, vector, sound, integer, flag, entity};
    graph.connections = {{1, 0, 2, 0}, {2, 0, 4, 0}};

    FunctionGraph function;
    function.name = "Twice";
    function.returnType = PinKind::Float;
    function.parameters = {{"value", PinKind::Float, {}}};
    function.nodes = {Node(10, ScriptNodeType::ReturnValue, {Pin(PinKind::Float, 2.0f)}, {})};
    graph.functions = {function};
    graph.customEvents = {{"BossDefeated", {{"reward", PinKind::Int, {}}}}};

    const std::string text = VisualScriptGraphIO::Serialize(graph);
    auto parsed = VisualScriptGraphIO::Parse(text);
    if (!parsed)
        std::printf("  %s\n", parsed.error().c_str());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(VisualScriptGraphIO::Serialize(*parsed), text);

    EXPECT_EQ(parsed->className, graph.className);
    EXPECT_EQ(parsed->description, graph.description);
    ASSERT_EQ(parsed->variables.size(), static_cast<size_t>(3));
    EXPECT_EQ(parsed->variables[0].defaultValue, std::string("2.5f"));
    EXPECT_TRUE(parsed->variables[1].type == PinKind::Bool);
    ASSERT_EQ(parsed->nodes.size(), graph.nodes.size());
    EXPECT_EQ(parsed->nodes[0].editorX, -12.5f);
    EXPECT_EQ(parsed->nodes[0].editorY, 0.7071068f); // shortest float text parses back exactly
    EXPECT_EQ(parsed->nodes[1].inputs[1].defaultString, std::string("a\tb"));
    EXPECT_EQ(parsed->nodes[2].outputs[0].defaultValue[2], 0.1f);
    EXPECT_EQ(parsed->nodes[3].properties.at("bus"), std::string("sfx"));
    EXPECT_EQ(parsed->nodes[4].outputs[0].defaultValue[0], -42.0f);
    EXPECT_EQ(parsed->nodes[5].outputs[0].defaultValue[0], 1.0f);
    EXPECT_EQ(parsed->nodes[6].outputs[0].defaultValue[0], 9.0f);
    ASSERT_EQ(parsed->connections.size(), static_cast<size_t>(2));
    EXPECT_EQ(parsed->connections[1].toNode, 4u);
    ASSERT_EQ(parsed->functions.size(), static_cast<size_t>(1));
    EXPECT_EQ(parsed->functions[0].nodes[0].inputs[0].defaultValue[0], 2.0f);
    ASSERT_EQ(parsed->customEvents.size(), static_cast<size_t>(1));
    EXPECT_TRUE(parsed->customEvents[0].parameters[0].type == PinKind::Int);

    // Every node type has a stable file name that maps back to it.
    for (const auto& entry : VisualScriptCompiler::GetNodePalette())
    {
        const char* name = VisualScriptGraphIO::NodeTypeName(entry.type);
        ASSERT_TRUE(name != nullptr);
        EXPECT_TRUE(VisualScriptGraphIO::NodeTypeFromName(name) == entry.type);
    }
}

TEST(VisualScriptGraphs_ParserRejectsMalformedGraphs)
{
    ASSERT_TRUE(VisualScriptGraphIO::Parse(ValidDocument()).has_value());

    struct Case
    {
        std::string text;
        const char* expected;
    };
    auto replace = [](std::string text, const std::string& from, const std::string& to)
    {
        const size_t at = text.find(from);
        return at == std::string::npos ? std::string("<pattern missing: ") + from + ">"
                                       : text.replace(at, from.size(), to);
    };
    const std::string valid = ValidDocument();
    const std::vector<Case> cases = {
        {"{not json", "not valid JSON"},
        {replace(valid, "spark.vscript", "spark.graph"), "format"},
        {replace(valid, "\"version\": 1", "\"version\": 2"), "unsupported .vscript version 2"},
        {ValidDocument(R"(, "extra": 1)"), "unknown key 'extra'"},
        {replace(valid, "\"Probe\"", "\"Bad Name\""), "not an identifier"},
        {replace(valid, "\"PrintMessage\"", "\"PrintMesage\""), "unknown node type 'PrintMesage'"},
        {replace(valid, "\"id\": 2", "\"id\": 1"), "duplicate node id 1"},
        {replace(valid, "\"id\": 2", "\"id\": 0"), "0 is reserved"},
        {replace(valid, "\"String\", \"default\": \"hi\"", "\"Strng\""), "unknown pin kind 'Strng'"},
        {replace(valid, "\"String\", \"default\": \"hi\"", "\"Float\", \"default\": true"), "must be a number"},
        {replace(valid, "\"to\": [2, 0]", "\"to\": [3, 0]"), "names missing node 3"},
        {replace(valid, "\"to\": [2, 0]", "\"to\": [2, 5]"), "has no input pin 5"},
        {replace(valid, "\"to\": [2, 0]", "\"to\": [2, 1]"),
         "cannot wire an output of kind Execution to an input of kind String"},
        {ValidDocument(R"(, "functions": [], "customEvents": [], "description": 3)"), "description: must be a string"},
        {replace(valid, R"({"from": [1, 0], "to": [2, 0]})",
                 R"({"from": [1, 0], "to": [2, 0]}, {"from": [1, 0], "to": [2, 0]})"),
         "already has a wire"},
        {replace(valid, R"("variables": [])",
                 R"("variables": [{"name": "a", "type": "Float"}, {"name": "a", "type": "Int"}])"),
         "duplicate variable 'a'"},
        {replace(valid, R"("variables": [])", R"("variables": [{"name": "a", "type": "Execution"}])"),
         "not a value type"},
        {replace(valid, "\"String\", \"default\": \"hi\"", "\"Int\", \"default\": 33554432"),
         "must be an integer in [-16777216, 16777216]"},
        // A repeated key would otherwise silently keep only its last value.
        {ValidDocument(R"(, "nodes": [])"), "duplicate object key"},
        {replace(valid, R"("type": "OnStart")", R"("type": "OnStart", "type": "OnStart")"), "duplicate object key"},
        {replace(valid, "\"version\": 1", "\"version\": 1.5"), "unsupported .vscript version 1.5"},
        // Ids stay far from UINT32_MAX so the editor's next id can never wrap onto an existing node.
        {replace(valid, "\"id\": 2", "\"id\": 16777216"), "exceeds the largest node id 16777215"},
        // One execution output feeds one input; the editor replaces the old wire rather than fanning out.
        {replace(replace(valid, R"([{"kind": "Execution"}]}])",
                         R"([{"kind": "Execution"}]}, {"id": 3, "type": "PrintMessage", "inputs": [{"kind": )"
                         R"("Execution"}, {"kind": "String"}]}])"),
                 R"({"from": [1, 0], "to": [2, 0]})",
                 R"({"from": [1, 0], "to": [2, 0]}, {"from": [1, 0], "to": [3, 0]})"),
         "execution output 0 of node 1 already has a wire"},
    };
    for (const auto& testCase : cases)
    {
        const auto parsed = VisualScriptGraphIO::Parse(testCase.text);
        EXPECT_FALSE(parsed.has_value());
        if (!parsed)
        {
            if (parsed.error().find(testCase.expected) == std::string::npos)
                std::printf("  expected '%s' in '%s'\n", testCase.expected, parsed.error().c_str());
            EXPECT_STR_CONTAINS(parsed.error(), testCase.expected);
        }
    }
}

TEST(VisualScriptGraphs_SaveFileIsAtomicAndRefusesUnloadableGraphs)
{
    // Unique per process and run: concurrent runs of this binary must never share (and delete) one directory.
    const fs::path directory =
        fs::temp_directory_path() / ("spark-vscript-" + std::to_string(std::random_device{}()) + "-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const fs::path path = directory / "Probe.vscript";

    auto graph = VisualScriptGraphIO::Parse(ValidDocument());
    ASSERT_TRUE(graph.has_value());
    ASSERT_TRUE(VisualScriptGraphIO::SaveFile(path, *graph).has_value());
    const std::string saved = ReadBytes(path);

    auto loaded = VisualScriptGraphIO::LoadFile(path);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(SameGraph(*loaded, *graph));

    // A graph that could not be loaded again is refused and the existing file is kept.
    VisualScriptGraph broken = *graph;
    broken.nodes.push_back(broken.nodes.front()); // duplicate node id
    const auto refused = VisualScriptGraphIO::SaveFile(path, broken);
    EXPECT_FALSE(refused.has_value());
    if (!refused)
        EXPECT_STR_CONTAINS(refused.error(), "duplicate node id");
    EXPECT_EQ(ReadBytes(path), saved);
    EXPECT_FALSE(fs::exists(directory / "Probe.vscript.tmp"));

    const auto missing = VisualScriptGraphIO::LoadFile(directory / "Missing.vscript");
    EXPECT_FALSE(missing.has_value());
    if (!missing)
        EXPECT_STR_CONTAINS(missing.error(), "Missing.vscript");

    std::error_code ignored;
    fs::remove_all(directory, ignored);
}

TEST(VisualScriptGraphs_CompilerRejectsCyclesAndNonLiteralDefaults)
{
    VisualScriptGraph cycle;
    cycle.className = "Cycle";
    cycle.nodes = {Node(1, ScriptNodeType::OnStart, {}, {Pin(PinKind::Execution)}),
                   Node(2, ScriptNodeType::DoNothing, {Pin(PinKind::Execution)}, {Pin(PinKind::Execution)}),
                   Node(3, ScriptNodeType::DoNothing, {Pin(PinKind::Execution)}, {Pin(PinKind::Execution)})};
    cycle.connections = {{1, 0, 2, 0}, {2, 0, 3, 0}, {3, 0, 2, 0}};
    auto result = VisualScriptCompiler::Compile(cycle);
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_STR_CONTAINS(result.errors.front(), "Execution cycle through node 2");

    VisualScriptGraph dataCycle;
    dataCycle.className = "DataCycle";
    dataCycle.nodes = {
        Node(1, ScriptNodeType::OnStart, {}, {Pin(PinKind::Execution)}),
        Node(2, ScriptNodeType::PrintMessage, {Pin(PinKind::Execution), Pin(PinKind::String)},
             {Pin(PinKind::Execution)}),
        Node(3, ScriptNodeType::AppendString, {Pin(PinKind::String), Pin(PinKind::Any)}, {Pin(PinKind::String)}),
        Node(4, ScriptNodeType::AppendString, {Pin(PinKind::String), Pin(PinKind::Any)}, {Pin(PinKind::String)})};
    dataCycle.connections = {{1, 0, 2, 0}, {3, 0, 2, 1}, {4, 0, 3, 0}, {3, 0, 4, 0}};
    result = VisualScriptCompiler::Compile(dataCycle);
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_STR_CONTAINS(result.errors.front(), "Data cycle");

    // A variable default is spliced after '=', so anything but a literal of its type is refused.
    VisualScriptGraph injected;
    injected.className = "Injected";
    injected.nodes = {Node(1, ScriptNodeType::OnStart, {}, {Pin(PinKind::Execution)})};
    injected.variables = {{"score", PinKind::Int, "0; void Pwned() {}"}};
    result = VisualScriptCompiler::Compile(injected);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.angelScriptSource.find("Pwned") == std::string::npos);
    for (const auto& [kind, literal] : std::vector<std::pair<PinKind, std::string>>{{PinKind::Float, "-1.5e-3f"},
                                                                                    {PinKind::Int, "-7"},
                                                                                    {PinKind::Bool, "false"},
                                                                                    {PinKind::String, "\"a\\\"b\""},
                                                                                    {PinKind::Entity, "3"}})
    {
        injected.variables = {{"value", kind, literal}};
        EXPECT_TRUE(VisualScriptCompiler::Compile(injected).success);
    }
}

TEST(VisualScriptGraphs_FunctionBodyEmitsEachStatementOnce)
{
    // Function body: Sequence -> [Branch(false) -> print("only-if-true")], [print("after")].
    // The previous compiler emitted every statement node in list order after already
    // nesting Branch/Sequence targets, so the print guarded by the Branch also ran
    // unconditionally. Nodes are listed targets-first to show list order no longer drives emission.
    const auto exec = [] { return Pin(PinKind::Execution); };
    FunctionGraph function;
    function.name = "Report";
    function.returnType = PinKind::Execution;
    function.nodes = {
        Node(3, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String, 0, "only-if-true")}, {exec()}),
        Node(4, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String, 0, "after")}, {exec()}),
        Node(1, ScriptNodeType::Sequence, {exec()}, {exec(), exec()}),
        Node(2, ScriptNodeType::Branch, {exec(), Pin(PinKind::Bool, 0.0f)}, {exec(), exec()}),
    };
    function.connections = {{1, 0, 2, 0}, {2, 0, 3, 0}, {1, 1, 4, 0}};

    VisualScriptGraph graph;
    graph.className = "FunctionOnce";
    graph.nodes = {Node(1, ScriptNodeType::OnStart, {}, {exec()})};
    graph.functions = {function};
    auto result = VisualScriptCompiler::Compile(graph);
    ASSERT_TRUE(result.success);
    const auto count = [&](std::string_view needle)
    {
        size_t hits = 0;
        for (size_t at = result.angelScriptSource.find(needle); at != std::string::npos;
             at = result.angelScriptSource.find(needle, at + 1))
            ++hits;
        return hits;
    };
    EXPECT_EQ(count("print(\"only-if-true\");"), static_cast<size_t>(1));
    EXPECT_EQ(count("print(\"after\");"), static_cast<size_t>(1));
    EXPECT_EQ(count("if (false)"), static_cast<size_t>(1));

    // A body whose statements all sit on an execution cycle has no entry and is refused.
    graph.functions[0].connections.push_back({4, 0, 1, 0});
    graph.functions[0].connections.push_back({3, 0, 4, 0});
    result = VisualScriptCompiler::Compile(graph);
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_STR_CONTAINS(result.errors.front(), "no entry statement");
}

TEST(VisualScriptGraphs_HostileDiamondFailsInBoundedTime)
{
    // 48 layers of two Sequence nodes, each wired to both nodes of the next layer:
    // 2^48 execution paths. The statement cap must stop the walk itself, not only
    // the output, so the compile fails promptly instead of running for years.
    constexpr uint32_t kLayers = 48;
    const auto exec = [] { return Pin(PinKind::Execution); };
    VisualScriptGraph graph;
    graph.className = "Diamond";
    graph.nodes = {Node(1, ScriptNodeType::OnStart, {}, {exec()})};
    const auto nodeId = [](uint32_t layer, uint32_t side) { return 2 + layer * 2 + side; };
    for (uint32_t layer = 0; layer < kLayers; ++layer)
    {
        for (uint32_t side = 0; side < 2; ++side)
        {
            graph.nodes.push_back(Node(nodeId(layer, side), ScriptNodeType::Sequence, {exec()}, {exec(), exec()}));
            if (layer + 1 < kLayers)
            {
                graph.connections.push_back({nodeId(layer, side), 0, nodeId(layer + 1, 0), 0});
                graph.connections.push_back({nodeId(layer, side), 1, nodeId(layer + 1, 1), 0});
            }
        }
    }
    graph.connections.push_back({1, 0, nodeId(0, 0), 0});

    const auto started = std::chrono::steady_clock::now();
    const auto result = VisualScriptCompiler::Compile(graph);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_STR_CONTAINS(result.errors.front(), "Graph expands to more than");
    // Uncapped this walk never finishes; capped it is about one million steps. The bound
    // leaves wide headroom for sanitizer and coverage builds.
    EXPECT_TRUE(elapsed < std::chrono::seconds(60));
}

TEST(VisualScriptGraphs_CustomEventNodesMustNameADeclaredEvent)
{
    // Custom event chains used to be merged into one CustomHandler() nothing called, whatever
    // event they named. Each chain now compiles into its declared event's method, so a node
    // naming no declared event (or none at all) is a compile error rather than dead code.
    const auto exec = [] { return Pin(PinKind::Execution); };
    VisualScriptGraph graph;
    graph.className = "CustomEvents";
    graph.customEvents = {{"Scored", {{"points", PinKind::Int, {}}}}};
    graph.nodes = {Node(1, ScriptNodeType::OnCustomEvent, {}, {exec()}, {{"event", "Scored"}}),
                   Node(2, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String, 0, "scored")}, {exec()})};
    graph.connections = {{1, 0, 2, 0}};
    auto result = VisualScriptCompiler::Compile(graph);
    ASSERT_TRUE(result.success);
    EXPECT_STR_CONTAINS(result.angelScriptSource, "void OnScored(int points)\n    {\n        print(\"scored\");");
    EXPECT_TRUE(result.angelScriptSource.find("CustomHandler") == std::string::npos);
    EXPECT_EQ(CountOccurrences(result.angelScriptSource, "void OnScored("), static_cast<size_t>(1));

    using Properties = std::unordered_map<std::string, std::string>;
    for (const Properties& properties : {Properties{{"event", "Missed"}}, Properties{}})
    {
        graph.nodes[0].properties = properties;
        result = VisualScriptCompiler::Compile(graph);
        EXPECT_FALSE(result.success);
        ASSERT_FALSE(result.errors.empty());
        EXPECT_STR_CONTAINS(result.errors.front(), "Custom event node 1 names no declared custom event");
    }

    // Two declarations that sanitize to one method name would otherwise define it twice.
    graph.nodes[0].properties = {{"event", "Scored"}};
    graph.customEvents.push_back({"Scored", {}});
    result = VisualScriptCompiler::Compile(graph);
    EXPECT_FALSE(result.success);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_STR_CONTAINS(result.errors.front(), "generates method OnScored twice");
}

TEST(VisualScriptGraphs_PropertyFallbackIgnoresHashOrder)
{
    // A node without its canonical property key falls back to another property. Taking
    // unordered_map::begin() made that pick depend on insertion order and the standard
    // library; the smallest key is taken instead, whatever the map's iteration order.
    const auto exec = [] { return Pin(PinKind::Execution); };
    const auto sound = [&](uint32_t id, const std::vector<std::pair<std::string, std::string>>& properties)
    {
        ScriptNode node = Node(id, ScriptNodeType::PlaySound, {exec()}, {exec()});
        for (const auto& [key, value] : properties)
            node.properties.emplace(key, value);
        return node;
    };
    VisualScriptGraph graph;
    graph.className = "Fallback";
    graph.nodes = {Node(1, ScriptNodeType::OnStart, {}, {exec()}),
                   sound(2, {{"mix", "m"}, {"clip", "c"}, {"volume", "v"}}),
                   sound(3, {{"volume", "v"}, {"clip", "c"}, {"mix", "m"}})};
    graph.connections = {{1, 0, 2, 0}, {2, 0, 3, 0}};
    const auto result = VisualScriptCompiler::Compile(graph);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(CountOccurrences(result.angelScriptSource, "playSound(selfEntity, \"c\");"), static_cast<size_t>(2));
}

#ifdef SPARK_ANGELSCRIPT_SUPPORT
namespace
{
    /// Compiles a graph into the production AngelScriptEngine, runs Start(), and captures print() output.
    struct CompiledGraphRun
    {
        ScopedLoggerBaseline loggerBaseline;
        std::vector<std::string> printed;
        AngelScriptEngine engine;
        World world;
        std::string source;
        bool ran = false;

        explicit CompiledGraphRun(const VisualScriptGraph& graph)
        {
            Spark::Logger::Get().AddSink(std::make_unique<Spark::CallbackSink>(
                [this](const Spark::LogMessage& message)
                {
                    constexpr std::string_view prefix = "[Script] ";
                    if (message.message.starts_with(prefix))
                        printed.push_back(message.message.substr(prefix.size()));
                }));
            const auto compiled = VisualScriptCompiler::Compile(graph);
            source = compiled.angelScriptSource;
            if (!compiled.success || !engine.Initialize())
                return;
            AngelScriptEngine::BindWorld(&world);
            if (!engine.CompileScriptFromString(source, graph.className))
            {
                std::printf("  %s\n%s\n", engine.GetLastError().c_str(), source.c_str());
                return;
            }
            const EntityID entity = world.CreateEntity("GraphProbe");
            if (!engine.AttachScript(entity, graph.className, graph.className))
                return;
            engine.CallStart(entity);
            ran = !engine.IsScriptFaulted(entity);
            engine.DetachScript(entity);
        }

        ~CompiledGraphRun()
        {
            AngelScriptEngine::BindWorld(nullptr);
            engine.Shutdown();
            Spark::Logger::Get().ClearSinks(); // drop the sink capturing `this` before it dangles
        }

        CompiledGraphRun(const CompiledGraphRun&) = delete;
        CompiledGraphRun& operator=(const CompiledGraphRun&) = delete;
    };
} // namespace

TEST(VisualScriptGraphs_DataNodesEvaluateAtTheirPointOfUse)
{
    // Start: counter = 1; counter = counter + 1; print("counter=" + counter).
    // One GetVariable node feeds both the Add and the Append. The previous
    // compiler hoisted every data node to the top of the method, so it read
    // counter before the first write ("float n3_out0 = counter; counter = 1;
    // ... counter = n3_out0 + 1"), leaving 1. Evaluated where each statement
    // uses it, the chain leaves 2 and prints "counter=2".
    VisualScriptGraph graph;
    graph.className = "PointOfUse";
    graph.variables = {{"counter", PinKind::Float, "0.0f"}};
    graph.nodes = {
        Node(1, ScriptNodeType::OnStart, {}, {Pin(PinKind::Execution)}),
        Node(2, ScriptNodeType::SetVariable, {Pin(PinKind::Execution), Pin(PinKind::Float, 1.0f)},
             {Pin(PinKind::Execution)}, {{"name", "counter"}}),
        Node(3, ScriptNodeType::GetVariable, {}, {Pin(PinKind::Float)}, {{"name", "counter"}}),
        Node(4, ScriptNodeType::Add, {Pin(PinKind::Float), Pin(PinKind::Float, 1.0f)}, {Pin(PinKind::Float)}),
        Node(5, ScriptNodeType::SetVariable, {Pin(PinKind::Execution), Pin(PinKind::Float)}, {Pin(PinKind::Execution)},
             {{"name", "counter"}}),
        Node(6, ScriptNodeType::AppendString, {Pin(PinKind::String, 0, "counter="), Pin(PinKind::Any)},
             {Pin(PinKind::String)}),
        Node(7, ScriptNodeType::PrintMessage, {Pin(PinKind::Execution), Pin(PinKind::String)},
             {Pin(PinKind::Execution)}),
    };
    graph.connections = {{1, 0, 2, 0}, {2, 0, 5, 0}, {3, 0, 4, 0}, {4, 0, 5, 1},
                         {5, 0, 7, 0}, {3, 0, 6, 1}, {6, 0, 7, 1}};

    CompiledGraphRun run(graph);
    EXPECT_TRUE(run.ran);
    ASSERT_EQ(run.printed.size(), static_cast<size_t>(1));
    EXPECT_EQ(run.printed[0], std::string("counter=2"));
}

TEST(VisualScriptGraphs_NestedControlFlowRunsInAngelScript)
{
    // for (i = 0; i < 4; i++) { Sequence: [if (i < 1) "low" else if (i < 3) "mid" else "high"], [print i] }
    // then, after the loop, "done". Exercises nested Branch inside a Branch's
    // False output, Sequence inside a ForLoop body, and the ForLoop Completed pin.
    VisualScriptGraph graph;
    graph.className = "NestedFlow";
    const auto exec = [] { return Pin(PinKind::Execution); };
    const auto print = [&](uint32_t id, const char* text)
    { return Node(id, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String, 0, text)}, {exec()}); };
    graph.nodes = {
        Node(1, ScriptNodeType::OnStart, {}, {exec()}),
        Node(2, ScriptNodeType::ForLoop, {exec(), Pin(PinKind::Int, 0.0f), Pin(PinKind::Int, 4.0f)},
             {exec(), Pin(PinKind::Int), exec()}),
        Node(3, ScriptNodeType::Sequence, {exec()}, {exec(), exec()}),
        Node(4, ScriptNodeType::Less, {Pin(PinKind::Float), Pin(PinKind::Float, 1.0f)}, {Pin(PinKind::Bool)}),
        Node(5, ScriptNodeType::Branch, {exec(), Pin(PinKind::Bool)}, {exec(), exec()}),
        print(6, "low"),
        Node(7, ScriptNodeType::Less, {Pin(PinKind::Float), Pin(PinKind::Float, 3.0f)}, {Pin(PinKind::Bool)}),
        Node(8, ScriptNodeType::Branch, {exec(), Pin(PinKind::Bool)}, {exec(), exec()}),
        print(9, "mid"),
        print(10, "high"),
        Node(11, ScriptNodeType::AppendString, {Pin(PinKind::String, 0, "i="), Pin(PinKind::Any)},
             {Pin(PinKind::String)}),
        Node(12, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String)}, {exec()}),
        print(13, "done"),
    };
    graph.connections = {
        {1, 0, 2, 0},  {2, 0, 3, 0},  {3, 0, 5, 0},  {2, 1, 4, 0},   {4, 0, 5, 1},
        {5, 0, 6, 0},  {5, 1, 8, 0},  {2, 1, 7, 0},  {7, 0, 8, 1},   {8, 0, 9, 0},
        {8, 1, 10, 0}, {3, 1, 12, 0}, {2, 1, 11, 1}, {11, 0, 12, 1}, {2, 2, 13, 0},
    };

    CompiledGraphRun run(graph);
    EXPECT_TRUE(run.ran);
    const std::vector<std::string> expected = {"low", "i=0", "mid", "i=1", "mid", "i=2", "high", "i=3", "done"};
    EXPECT_EQ(run.printed.size(), expected.size());
    for (size_t i = 0; i < expected.size() && i < run.printed.size(); ++i)
        EXPECT_EQ(run.printed[i], expected[i]);
}
TEST(VisualScriptGraphs_FunctionGraphsRunInAngelScript)
{
    // Report(): Sequence -> [Branch(false) -> "only-if-true" else "only-if-false"], ["after"].
    // Pick(bool flag): if (flag) return 7; else return 3 -- through ReturnValue's value input.
    // Start: Report(); print("pick=" + Pick(true)); print("pick=" + Pick(false)).
    const auto exec = [] { return Pin(PinKind::Execution); };
    const auto print = [&](uint32_t id, const char* text)
    { return Node(id, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String, 0, text)}, {exec()}); };

    FunctionGraph report;
    report.name = "Report";
    report.returnType = PinKind::Execution;
    report.nodes = {
        print(3, "only-if-true"),
        print(4, "only-if-false"),
        print(5, "after"),
        Node(1, ScriptNodeType::Sequence, {exec()}, {exec(), exec()}),
        Node(2, ScriptNodeType::Branch, {exec(), Pin(PinKind::Bool, 0.0f)}, {exec(), exec()}),
    };
    report.connections = {{1, 0, 2, 0}, {2, 0, 3, 0}, {2, 1, 4, 0}, {1, 1, 5, 0}};

    FunctionGraph pick;
    pick.name = "Pick";
    pick.returnType = PinKind::Int;
    pick.parameters = {{"flag", PinKind::Bool, ""}};
    pick.nodes = {
        Node(1, ScriptNodeType::GetVariable, {}, {Pin(PinKind::Bool)}, {{"name", "flag"}}),
        Node(2, ScriptNodeType::Branch, {exec(), Pin(PinKind::Bool)}, {exec(), exec()}),
        Node(3, ScriptNodeType::ReturnValue, {exec(), Pin(PinKind::Int, 7.0f)}, {}),
        Node(4, ScriptNodeType::ReturnValue, {exec(), Pin(PinKind::Int, 3.0f)}, {}),
    };
    pick.connections = {{1, 0, 2, 1}, {2, 0, 3, 0}, {2, 1, 4, 0}};

    const auto call =
        [&](uint32_t id, const char* function, std::vector<ScriptPin> inputs, std::vector<ScriptPin> outputs)
    { return Node(id, ScriptNodeType::CallFunction, std::move(inputs), std::move(outputs), {{"function", function}}); };
    const auto append = [](uint32_t id)
    {
        return Node(id, ScriptNodeType::AppendString, {Pin(PinKind::String, 0, "pick="), Pin(PinKind::Any)},
                    {Pin(PinKind::String)});
    };

    VisualScriptGraph graph;
    graph.className = "FunctionFlow";
    graph.functions = {report, pick};
    graph.nodes = {
        Node(1, ScriptNodeType::OnStart, {}, {exec()}),
        call(2, "Report", {exec()}, {exec()}),
        call(3, "Pick", {exec(), Pin(PinKind::Bool, 1.0f)}, {exec(), Pin(PinKind::Int)}),
        append(4),
        Node(5, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String)}, {exec()}),
        call(6, "Pick", {exec(), Pin(PinKind::Bool, 0.0f)}, {exec(), Pin(PinKind::Int)}),
        append(7),
        Node(8, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String)}, {exec()}),
    };
    graph.connections = {{1, 0, 2, 0}, {2, 0, 3, 0}, {3, 0, 5, 0}, {3, 1, 4, 1}, {4, 0, 5, 1},
                         {5, 0, 6, 0}, {6, 0, 8, 0}, {6, 1, 7, 1}, {7, 0, 8, 1}};

    CompiledGraphRun run(graph);
    EXPECT_TRUE(run.ran);
    const std::vector<std::string> expected = {"only-if-false", "after", "pick=7", "pick=3"};
    EXPECT_EQ(run.printed.size(), expected.size());
    for (size_t i = 0; i < expected.size() && i < run.printed.size(); ++i)
        EXPECT_EQ(run.printed[i], expected[i]);
    if (!run.ran || run.printed != expected)
        std::printf("%s\n", run.source.c_str());
}

TEST(VisualScriptGraphs_Vector3LiteralsCompileInAngelScript)
{
    // A ConstVector3 node and every unwired Vector3 input compile to Vector3(x, y, z), which
    // the script API did not register, so any such graph compiled here but not in AngelScript.
    const auto exec = [] { return Pin(PinKind::Execution); };
    const auto print = [&](uint32_t id)
    { return Node(id, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String)}, {exec()}); };
    const auto append = [](uint32_t id, const char* prefix)
    {
        return Node(id, ScriptNodeType::AppendString, {Pin(PinKind::String, 0, prefix), Pin(PinKind::Any)},
                    {Pin(PinKind::String)});
    };
    const auto breakVector = [](uint32_t id, ScriptPin input)
    {
        return Node(id, ScriptNodeType::BreakVector3, {std::move(input)},
                    {Pin(PinKind::Float), Pin(PinKind::Float), Pin(PinKind::Float)});
    };

    VisualScriptGraph graph;
    graph.className = "VectorLiterals";
    graph.nodes = {
        Node(1, ScriptNodeType::OnStart, {}, {exec()}),
        Node(2, ScriptNodeType::ConstVector3, {}, {Vector3Pin(1.5f, -2.0f, 0.25f)}),
        breakVector(3, Pin(PinKind::Vector3)),
        append(4, "y="),
        print(5),
        // Unwired Vector3 inputs: the position of SetPosition and the vector of a BreakVector3.
        Node(6, ScriptNodeType::SetPosition, {exec(), Pin(PinKind::Entity), Vector3Pin(4.0f, 5.0f, 6.0f)}, {exec()}),
        breakVector(7, Vector3Pin(4.0f, 5.0f, 6.0f)),
        append(8, "z="),
        print(9),
    };
    graph.connections = {{1, 0, 5, 0}, {2, 0, 3, 0}, {3, 1, 4, 1}, {4, 0, 5, 1},
                         {5, 0, 6, 0}, {6, 0, 9, 0}, {7, 2, 8, 1}, {8, 0, 9, 1}};

    CompiledGraphRun run(graph);
    EXPECT_STR_CONTAINS(run.source, "Vector3(1.500000f, -2.000000f, 0.250000f)");
    EXPECT_TRUE(run.ran);
    const std::vector<std::string> expected = {"y=-2", "z=6"};
    EXPECT_TRUE(run.printed == expected);
    if (!run.ran || run.printed != expected)
        std::printf("%s\n", run.source.c_str());
}

TEST(VisualScriptGraphs_CustomEventChainsRunInAngelScript)
{
    // Start raises Scored(3) with CallFunction; both OnCustomEvent("Scored") chains run in
    // OnScored(int points) and read the parameter. The declared, unhandled Idle event still
    // gets its method, so raising it compiles and does nothing.
    const auto exec = [] { return Pin(PinKind::Execution); };
    const auto onScored = [&](uint32_t id)
    { return Node(id, ScriptNodeType::OnCustomEvent, {}, {exec()}, {{"event", "Scored"}}); };
    const auto raise = [&](uint32_t id, const char* method, std::vector<ScriptPin> inputs)
    { return Node(id, ScriptNodeType::CallFunction, std::move(inputs), {exec()}, {{"function", method}}); };

    VisualScriptGraph graph;
    graph.className = "CustomEventFlow";
    graph.customEvents = {{"Scored", {{"points", PinKind::Int, {}}}}, {"Idle", {}}};
    graph.nodes = {
        Node(1, ScriptNodeType::OnStart, {}, {exec()}),
        raise(2, "OnScored", {exec(), Pin(PinKind::Int, 3.0f)}),
        raise(3, "OnIdle", {exec()}),
        onScored(4),
        Node(5, ScriptNodeType::GetVariable, {}, {Pin(PinKind::Int)}, {{"name", "points"}}),
        Node(6, ScriptNodeType::AppendString, {Pin(PinKind::String, 0, "points="), Pin(PinKind::Any)},
             {Pin(PinKind::String)}),
        Node(7, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String)}, {exec()}),
        onScored(8),
        Node(9, ScriptNodeType::PrintMessage, {exec(), Pin(PinKind::String, 0, "second handler")}, {exec()}),
    };
    graph.connections = {{1, 0, 2, 0}, {2, 0, 3, 0}, {4, 0, 7, 0}, {5, 0, 6, 1}, {6, 0, 7, 1}, {8, 0, 9, 0}};

    CompiledGraphRun run(graph);
    EXPECT_TRUE(run.ran);
    const std::vector<std::string> expected = {"points=3", "second handler"};
    EXPECT_TRUE(run.printed == expected);
    if (!run.ran || run.printed != expected)
        std::printf("%s\n", run.source.c_str());
}
#endif // SPARK_ANGELSCRIPT_SUPPORT
