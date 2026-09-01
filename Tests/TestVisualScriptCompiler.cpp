// TestVisualScriptCompiler.cpp - Tests for visual script graph to AngelScript compilation
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Real engine compiler — used below for the code-injection regression tests
// (VisualScriptCompiler_Security*) which must exercise the ACTUAL
// EscapeAngelScriptString/SanitizeIdentifier fix, not the standalone mirror
// implementation used by the rest of this file. VisualScriptCompiler.h/.cpp
// has no AngelScript SDK dependency (pure string/struct compiler), so it's
// safe to link directly like the rest of SparkEngineLib.
#include "Engine/Scripting/VisualScriptCompiler.h"
#include "../GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoRuntime.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT
#include <angelscript.h>
#include <scriptarray/scriptarray.h>
#include <scriptbuilder/scriptbuilder.h>
#include <scriptstdstring/scriptstdstring.h>
#endif

namespace TestVSC
{

    // ========================================================================
    // Standalone types (mirrors engine types)
    // ========================================================================

    enum class ScriptNodeType : uint32_t
    {
        OnStart = 0,
        OnUpdate = 1,
        OnKeyPress = 5,
        Branch = 50,
        GetKeyDown = 106,
        GetDeltaTime = 108,
        PrintMessage = 158,
        Add = 200,
        Multiply = 202,
        ConstFloat = 350,
        ConstString = 353,
    };

    enum class PinKind : uint8_t
    {
        Execution,
        Bool,
        Int,
        Float,
        String,
        Vector3,
        Entity,
        Any,
    };

    struct ScriptPin
    {
        PinKind kind = PinKind::Float;
        float defaultValue[4] = {};
        std::string defaultString;
        bool isConnected = false;
    };

    struct ScriptNode
    {
        uint32_t id = 0;
        ScriptNodeType type{};
        std::vector<ScriptPin> inputs;
        std::vector<ScriptPin> outputs;
        std::unordered_map<std::string, std::string> properties;
    };

    struct ScriptConnection
    {
        uint32_t fromNode = 0;
        uint32_t fromPin = 0;
        uint32_t toNode = 0;
        uint32_t toPin = 0;
    };

    struct VariableDecl
    {
        std::string name;
        PinKind type = PinKind::Float;
        std::string defaultValue;
    };

    struct VisualScriptGraph
    {
        std::string className = "TestScript";
        std::vector<ScriptNode> nodes;
        std::vector<ScriptConnection> connections;
        std::vector<VariableDecl> variables;
    };

    struct CompileResult
    {
        std::string source;
        std::vector<std::string> errors;
        bool success = false;
    };

    // ========================================================================
    // Minimal compiler for testing
    // ========================================================================

    bool IsEventNode(ScriptNodeType type)
    {
        return type == ScriptNodeType::OnStart || type == ScriptNodeType::OnUpdate ||
               type == ScriptNodeType::OnKeyPress;
    }

    const ScriptNode* FindNode(const VisualScriptGraph& graph, uint32_t id)
    {
        for (const auto& n : graph.nodes)
        {
            if (n.id == id)
                return &n;
        }
        return nullptr;
    }

    const ScriptConnection* FindInputConn(const VisualScriptGraph& graph, uint32_t nodeId, uint32_t pin)
    {
        for (const auto& c : graph.connections)
        {
            if (c.toNode == nodeId && c.toPin == pin)
                return &c;
        }
        return nullptr;
    }

    std::string VarName(uint32_t nodeId, uint32_t pin)
    {
        return "n" + std::to_string(nodeId) + "_out" + std::to_string(pin);
    }

    std::string PinTypeStr(PinKind k)
    {
        switch (k)
        {
        case PinKind::Bool:
            return "bool";
        case PinKind::Int:
            return "int";
        case PinKind::Float:
            return "float";
        case PinKind::String:
            return "string";
        default:
            return "float";
        }
    }

    std::vector<uint32_t> TopoSort(const VisualScriptGraph& graph, uint32_t start)
    {
        std::unordered_set<uint32_t> visited;
        std::vector<uint32_t> order;
        std::queue<uint32_t> q;
        q.push(start);
        visited.insert(start);
        while (!q.empty())
        {
            uint32_t cur = q.front();
            q.pop();
            for (const auto& c : graph.connections)
            {
                // Forward: follow execution/data flow
                if (c.fromNode == cur && visited.find(c.toNode) == visited.end())
                {
                    visited.insert(c.toNode);
                    q.push(c.toNode);
                }
                // Backward: follow data dependencies
                if (c.toNode == cur && visited.find(c.fromNode) == visited.end())
                {
                    visited.insert(c.fromNode);
                    q.push(c.fromNode);
                }
            }
            order.push_back(cur);
        }
        // Sort: producers before consumers
        std::sort(order.begin(), order.end(),
                  [&graph](uint32_t a, uint32_t b)
                  {
                      for (const auto& c : graph.connections)
                      {
                          if (c.fromNode == a && c.toNode == b)
                              return true;
                          if (c.fromNode == b && c.toNode == a)
                              return false;
                      }
                      return a < b;
                  });
        return order;
    }

    CompileResult Compile(const VisualScriptGraph& graph)
    {
        CompileResult result;

        if (graph.nodes.empty())
        {
            result.errors.push_back("Empty graph");
            return result;
        }

        std::vector<const ScriptNode*> eventNodes;
        for (const auto& n : graph.nodes)
        {
            if (IsEventNode(n.type))
                eventNodes.push_back(&n);
        }

        if (eventNodes.empty())
        {
            result.errors.push_back("No event nodes");
            return result;
        }

        std::ostringstream src;
        src << "class " << graph.className << "\n{\n";

        for (const auto& v : graph.variables)
        {
            src << "    " << PinTypeStr(v.type) << " " << v.name;
            if (!v.defaultValue.empty())
                src << " = " << v.defaultValue;
            src << ";\n";
        }

        for (const auto* evt : eventNodes)
        {
            std::string method = (evt->type == ScriptNodeType::OnStart)    ? "Start"
                                 : (evt->type == ScriptNodeType::OnUpdate) ? "Update"
                                                                           : "Handler";
            std::string params = (evt->type == ScriptNodeType::OnUpdate) ? "float dt" : "";

            src << "    void " << method << "(" << params << ")\n    {\n";

            auto sorted = TopoSort(graph, evt->id);
            for (uint32_t nid : sorted)
            {
                if (nid == evt->id)
                    continue;
                const auto* node = FindNode(graph, nid);
                if (!node || IsEventNode(node->type))
                    continue;

                if (node->type == ScriptNodeType::ConstFloat)
                {
                    float val = !node->outputs.empty() ? node->outputs[0].defaultValue[0] : 0.0f;
                    src << "        float " << VarName(nid, 0) << " = " << val << "f;\n";
                }
                else if (node->type == ScriptNodeType::Add)
                {
                    auto* c0 = FindInputConn(graph, nid, 0);
                    auto* c1 = FindInputConn(graph, nid, 1);
                    std::string a = c0 ? VarName(c0->fromNode, c0->fromPin) : "0.0f";
                    std::string b = c1 ? VarName(c1->fromNode, c1->fromPin) : "0.0f";
                    src << "        float " << VarName(nid, 0) << " = " << a << " + " << b << ";\n";
                }
                else if (node->type == ScriptNodeType::Multiply)
                {
                    auto* c0 = FindInputConn(graph, nid, 0);
                    auto* c1 = FindInputConn(graph, nid, 1);
                    std::string a = c0 ? VarName(c0->fromNode, c0->fromPin) : "0.0f";
                    std::string b = c1 ? VarName(c1->fromNode, c1->fromPin) : "0.0f";
                    src << "        float " << VarName(nid, 0) << " = " << a << " * " << b << ";\n";
                }
                else if (node->type == ScriptNodeType::PrintMessage)
                {
                    auto* c = FindInputConn(graph, nid, 0);
                    std::string msg = c ? VarName(c->fromNode, c->fromPin) : "\"Hello\"";
                    src << "        print(" << msg << ");\n";
                }
            }

            src << "    }\n";
        }

        src << "}\n";
        result.source = src.str();
        result.success = true;
        return result;
    }

} // namespace TestVSC

// ============================================================================
// Tests
// ============================================================================

TEST(VisualScriptCompiler_EmptyGraphFails)
{
    using namespace TestVSC;
    VisualScriptGraph graph;
    auto result = Compile(graph);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(!result.errors.empty());
}

TEST(VisualScriptCompiler_NoEventNodesFails)
{
    using namespace TestVSC;
    VisualScriptGraph graph;
    ScriptNode constNode;
    constNode.id = 1;
    constNode.type = ScriptNodeType::ConstFloat;
    ScriptPin out;
    out.kind = PinKind::Float;
    out.defaultValue[0] = 42.0f;
    constNode.outputs.push_back(out);
    graph.nodes.push_back(constNode);

    auto result = Compile(graph);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errors[0], std::string("No event nodes"));
}

TEST(VisualScriptCompiler_OnStartGeneratesMethod)
{
    using namespace TestVSC;
    VisualScriptGraph graph;
    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    auto result = Compile(graph);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.source.find("void Start()") != std::string::npos);
    EXPECT_TRUE(result.source.find("class TestScript") != std::string::npos);
}

TEST(VisualScriptCompiler_OnUpdateGeneratesMethod)
{
    using namespace TestVSC;
    VisualScriptGraph graph;
    ScriptNode update;
    update.id = 1;
    update.type = ScriptNodeType::OnUpdate;
    graph.nodes.push_back(update);

    auto result = Compile(graph);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.source.find("void Update(float dt)") != std::string::npos);
}

TEST(VisualScriptCompiler_ClassNameUsed)
{
    using namespace TestVSC;
    VisualScriptGraph graph;
    graph.className = "EnemyAI";
    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    auto result = Compile(graph);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.source.find("class EnemyAI") != std::string::npos);
}

TEST(VisualScriptCompiler_VariablesEmitted)
{
    using namespace TestVSC;
    VisualScriptGraph graph;
    graph.variables.push_back({"health", PinKind::Float, "100.0f"});
    graph.variables.push_back({"name", PinKind::String, ""});

    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    auto result = Compile(graph);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.source.find("float health = 100.0f") != std::string::npos);
    EXPECT_TRUE(result.source.find("string name") != std::string::npos);
}

TEST(VisualScriptCompiler_ConnectedMathNodes)
{
    using namespace TestVSC;
    VisualScriptGraph graph;

    // OnStart → (Add(ConstA, ConstB) → Print)
    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    ScriptNode constA;
    constA.id = 2;
    constA.type = ScriptNodeType::ConstFloat;
    ScriptPin outA;
    outA.kind = PinKind::Float;
    outA.defaultValue[0] = 3.0f;
    constA.outputs.push_back(outA);
    graph.nodes.push_back(constA);

    ScriptNode constB;
    constB.id = 3;
    constB.type = ScriptNodeType::ConstFloat;
    ScriptPin outB;
    outB.kind = PinKind::Float;
    outB.defaultValue[0] = 7.0f;
    constB.outputs.push_back(outB);
    graph.nodes.push_back(constB);

    ScriptNode addNode;
    addNode.id = 4;
    addNode.type = ScriptNodeType::Add;
    ScriptPin inA, inB;
    inA.kind = PinKind::Float;
    inA.isConnected = true;
    inB.kind = PinKind::Float;
    inB.isConnected = true;
    addNode.inputs = {inA, inB};
    ScriptPin addOut;
    addOut.kind = PinKind::Float;
    addNode.outputs.push_back(addOut);
    graph.nodes.push_back(addNode);

    // Connections: ConstA.0 → Add.0, ConstB.0 → Add.1
    graph.connections.push_back({2, 0, 4, 0}); // ConstA → Add input 0
    graph.connections.push_back({3, 0, 4, 1}); // ConstB → Add input 1
    // OnStart → Add (execution)
    graph.connections.push_back({1, 0, 4, 0});

    auto result = Compile(graph);
    EXPECT_TRUE(result.success);
    // Should contain the add expression
    EXPECT_TRUE(result.source.find("n2_out0") != std::string::npos);
    EXPECT_TRUE(result.source.find("n3_out0") != std::string::npos);
    EXPECT_TRUE(result.source.find("+") != std::string::npos);
}

TEST(VisualScriptCompiler_TopologicalSortOrder)
{
    using namespace TestVSC;
    VisualScriptGraph graph;

    ScriptNode n1{1, ScriptNodeType::OnStart, {}, {}, {}};
    ScriptNode n2{2, ScriptNodeType::ConstFloat, {}, {}, {}};
    ScriptNode n3{3, ScriptNodeType::Add, {}, {}, {}};
    graph.nodes = {n1, n2, n3};
    graph.connections.push_back({2, 0, 3, 0}); // Const → Add
    graph.connections.push_back({1, 0, 3, 0}); // OnStart → Add

    auto sorted = TopoSort(graph, 1);
    // n2 (dependency) should come before n3, and both after n1
    EXPECT_TRUE(sorted.size() >= 2);

    // Find positions
    int pos1 = -1, pos2 = -1, pos3 = -1;
    for (int i = 0; i < static_cast<int>(sorted.size()); ++i)
    {
        if (sorted[i] == 1)
            pos1 = i;
        if (sorted[i] == 2)
            pos2 = i;
        if (sorted[i] == 3)
            pos3 = i;
    }
    // Start node should come first, dependencies before dependents
    EXPECT_TRUE(pos1 < pos2);
    EXPECT_TRUE(pos2 < pos3);
}

TEST(VisualScriptCompiler_MultipleEventNodes)
{
    using namespace TestVSC;
    VisualScriptGraph graph;

    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    ScriptNode update;
    update.id = 2;
    update.type = ScriptNodeType::OnUpdate;
    graph.nodes.push_back(update);

    auto result = Compile(graph);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.source.find("void Start()") != std::string::npos);
    EXPECT_TRUE(result.source.find("void Update(float dt)") != std::string::npos);
}

TEST(VisualScriptCompiler_PrintMessageNode)
{
    using namespace TestVSC;
    VisualScriptGraph graph;

    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    ScriptNode printNode;
    printNode.id = 2;
    printNode.type = ScriptNodeType::PrintMessage;
    graph.nodes.push_back(printNode);

    graph.connections.push_back({1, 0, 2, 0});

    auto result = Compile(graph);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.source.find("print(") != std::string::npos);
}

TEST(VisualScriptCompiler_PinKindTypeStrings)
{
    using namespace TestVSC;
    EXPECT_EQ(PinTypeStr(PinKind::Bool), std::string("bool"));
    EXPECT_EQ(PinTypeStr(PinKind::Int), std::string("int"));
    EXPECT_EQ(PinTypeStr(PinKind::Float), std::string("float"));
    EXPECT_EQ(PinTypeStr(PinKind::String), std::string("string"));
}

TEST(VisualScriptCompiler_VarNameFormat)
{
    using namespace TestVSC;
    EXPECT_EQ(VarName(5, 0), std::string("n5_out0"));
    EXPECT_EQ(VarName(100, 2), std::string("n100_out2"));
}

// ============================================================================
// SparkGameVisualScript live-demo regressions
// ============================================================================

TEST(VisualScriptDemo_ManifestMatchesSpawnContract)
{
    uint32_t instances = 0;
    std::unordered_set<std::string> classNames;
    std::unordered_set<std::string> fileNames;
    for (const auto& asset : Spark::VisualScriptDemo::ScriptManifest)
    {
        instances += asset.instanceCount;
        classNames.emplace(asset.className);
        fileNames.emplace(asset.fileName);
    }

    EXPECT_EQ(instances, Spark::VisualScriptDemo::ExpectedEntityCount);
    EXPECT_EQ(classNames.size(), Spark::VisualScriptDemo::ScriptManifest.size());
    EXPECT_EQ(fileNames.size(), Spark::VisualScriptDemo::ScriptManifest.size());
}

TEST(VisualScriptDemo_SelectsFirstCompleteRootWithoutMixingDirectories)
{
    using namespace Spark::VisualScriptDemo;
    const std::array<std::filesystem::path, 3> roots = {"partial", "complete", "duplicate"};

    auto selected =
        SelectCompleteScriptRoot(roots,
                                 [](const std::filesystem::path& path)
                                 {
                                     if (path.parent_path() == "partial")
                                         return path.filename() != "HealthPickup.as";
                                     return path.parent_path() == "complete" || path.parent_path() == "duplicate";
                                 });

    EXPECT_TRUE(selected.has_value());
    EXPECT_EQ(selected->string(), std::filesystem::path("complete").string());
}

TEST(VisualScriptDemo_EntityBindingRequiresOnePlaceholder)
{
    using namespace Spark::VisualScriptDemo;
    auto bound = BindSelfEntity("class Demo { uint selfEntity = 0; void Start() {} }", 42);
    EXPECT_TRUE(bound.has_value());
    EXPECT_TRUE(bound->find("uint selfEntity = 42;") != std::string::npos);
    EXPECT_TRUE(bound->find(SelfEntityDeclaration) == std::string::npos);

    EXPECT_FALSE(BindSelfEntity("class Demo { void Start() {} }", 1).has_value());
    EXPECT_FALSE(BindSelfEntity("uint selfEntity = 0; uint selfEntity = 0;", 1).has_value());
}

TEST(VisualScriptDemo_DeltaTimeIsFinitePositiveAndBounded)
{
    using Spark::VisualScriptDemo::SanitizeDeltaTime;
    EXPECT_EQ(SanitizeDeltaTime(-0.01f), 0.0f);
    EXPECT_EQ(SanitizeDeltaTime(0.0f), 0.0f);
    EXPECT_EQ(SanitizeDeltaTime(std::numeric_limits<float>::infinity()), 0.0f);
    EXPECT_EQ(SanitizeDeltaTime(0.016f), 0.016f);
    EXPECT_EQ(SanitizeDeltaTime(0.5f), 0.1f);
}

TEST(VisualScriptDemo_RuntimeSupportContractIsExplicit)
{
    using namespace Spark::VisualScriptDemo;
    EXPECT_TRUE(EvaluateRuntimeSupport(true, true, true) == RuntimeSupport::Ready);
    EXPECT_TRUE(EvaluateRuntimeSupport(false, true, true) == RuntimeSupport::MissingCompiledSupport);
    EXPECT_TRUE(EvaluateRuntimeSupport(true, false, true) == RuntimeSupport::MissingWorld);
    EXPECT_TRUE(EvaluateRuntimeSupport(true, true, false) == RuntimeSupport::MissingScriptEngine);

    const std::string unsupported = std::string(RuntimeSupportMessage(RuntimeSupport::MissingCompiledSupport));
    EXPECT_TRUE(unsupported.find("ENABLE_ANGELSCRIPT") != std::string::npos);
    EXPECT_TRUE(unsupported.find("Module load rejected") != std::string::npos);
    EXPECT_TRUE(unsupported.find("vs_* commands are unavailable") != std::string::npos);

#ifdef SPARK_ANGELSCRIPT_SUPPORT
    EXPECT_TRUE(AngelScriptCompiledIn);
#else
    EXPECT_FALSE(AngelScriptCompiledIn);
#endif
}

TEST(VisualScriptDemo_GameplayLifecyclePublishesAngelScriptService)
{
    const auto lifecyclePath = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "SparkEngine" / "Source" / "Core" /
                               "Lifecycle" / "GameplayLifecycleShared.cpp";
    std::ifstream stream(lifecyclePath, std::ios::binary);
    EXPECT_TRUE(stream.is_open());
    if (!stream)
        return;

    std::ostringstream source;
    source << stream.rdbuf();
    EXPECT_TRUE(source.str().find("ctx->SetScriptEngine(&s_angelScript);") != std::string::npos);
}

TEST(VisualScriptDemo_ShippedScriptsUseBootstrapAPIOnly)
{
    const auto root = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "GameModules" / "SparkGameVisualScript" /
                      "Assets" / "Scripts" / "Generated";
    const std::array<std::string_view, 4> unsupportedCalls = {"sin(", "sqrt(", "atan2(", "Vector3("};

    for (const auto& asset : Spark::VisualScriptDemo::ScriptManifest)
    {
        std::ifstream stream(root / std::filesystem::path(asset.fileName), std::ios::binary);
        EXPECT_TRUE(stream.is_open());
        std::ostringstream source;
        source << stream.rdbuf();
        for (const auto call : unsupportedCalls)
            EXPECT_TRUE(source.str().find(call) == std::string::npos);
    }
}

#ifdef SPARK_ANGELSCRIPT_SUPPORT
static_assert(SPARK_ANGELSCRIPT_PACKED_POINTER_OPERAND == 1);
static_assert(alignof(AS_NAMESPACE_QUALIFIER asPWORD_UNALIGNED) == 1);

namespace
{
    class AngelScriptMemoryByteCodeStream final : public asIBinaryStream
    {
      public:
        int Write(const void* source, asUINT size) override
        {
            const auto* first = static_cast<const asBYTE*>(source);
            bytes.insert(bytes.end(), first, first + size);
            return 0;
        }

        int Read(void* destination, asUINT size) override
        {
            if (readOffset + size > bytes.size())
                return -1;
            std::memcpy(destination, bytes.data() + readOffset, size);
            readOffset += size;
            return 0;
        }

        std::vector<asBYTE> bytes;
        size_t readOffset = 0;
    };
} // namespace

TEST(VisualScriptDemo_AngelScriptPointerBytecodeOperandSupportsPackedStorage)
{
    alignas(asPWORD) asDWORD bytecode[AS_PTR_SIZE + 1] = {};
#if AS_PTR_SIZE == 2
    const asPWORD expected = static_cast<asPWORD>(0x123456789abcdef0ull);
#else
    const asPWORD expected = static_cast<asPWORD>(0x12345678u);
#endif

    asBC_PTRARG(bytecode) = expected;

    EXPECT_EQ(asBC_PTRARG(bytecode), expected);
}

TEST(VisualScriptDemo_AngelScriptCoreAndRequiredAddonsAreLinked)
{
    asIScriptEngine* engine = asCreateScriptEngine();
    EXPECT_TRUE(engine != nullptr);
    if (!engine)
        return;

    RegisterStdString(engine);
    RegisterScriptArray(engine, true);
    {
        CScriptBuilder builder;
        EXPECT_TRUE(builder.StartNewModule(engine, "VisualScriptLinkSmoke") >= 0);
        EXPECT_TRUE(
            builder.AddSectionFromMemory(
                "smoke", "class Smoke { array<string> values; void Start() { values.insertLast(\"ready\"); } }") >= 0);
        EXPECT_TRUE(builder.BuildModule() >= 0);
    }
    engine->ShutDownAndRelease();
}

TEST(VisualScriptDemo_AngelScriptPackedBytecodeRoundTripsObjectPointers)
{
    asIScriptEngine* engine = asCreateScriptEngine();
    EXPECT_TRUE(engine != nullptr);
    if (!engine)
        return;

    RegisterStdString(engine);
    RegisterScriptArray(engine, true);

    CScriptBuilder builder;
    EXPECT_TRUE(builder.StartNewModule(engine, "PackedBytecodeSource") >= 0);
    EXPECT_TRUE(builder.AddSectionFromMemory(
                    "packed-bytecode",
                    "class Persisted { array<string> values; Persisted() { values.insertLast(\"ready\"); } } "
                    "Persisted@ CreatePersisted() { return Persisted(); }") >= 0);
    EXPECT_TRUE(builder.BuildModule() >= 0);

    AngelScriptMemoryByteCodeStream stream;
    asIScriptModule* source = engine->GetModule("PackedBytecodeSource", asGM_ONLY_IF_EXISTS);
    EXPECT_TRUE(source != nullptr);
    if (source)
        EXPECT_TRUE(source->SaveByteCode(&stream) >= 0);
    EXPECT_TRUE(!stream.bytes.empty());

    asIScriptModule* restored = engine->GetModule("PackedBytecodeRestored", asGM_ALWAYS_CREATE);
    EXPECT_TRUE(restored != nullptr);
    if (restored && !stream.bytes.empty())
    {
        EXPECT_TRUE(restored->LoadByteCode(&stream) >= 0);
        EXPECT_TRUE(restored->GetFunctionByDecl("Persisted@ CreatePersisted()") != nullptr);
    }

    engine->ShutDownAndRelease();
}
#endif

// ============================================================================
// Security regression tests — exercise the REAL Spark::Scripting compiler
// (not the standalone mirror above) to prove crafted node properties can no
// longer inject AngelScript statements into generated source.
// ============================================================================

TEST(VisualScriptCompiler_Security_ConstStringEscapesQuotes)
{
    using namespace Spark::Scripting;
    VisualScriptGraph graph;

    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    ScriptNode constStr;
    constStr.id = 2;
    constStr.type = ScriptNodeType::ConstString;
    ScriptPin out;
    out.kind = PinKind::String;
    out.defaultString = "\"); fireEvent(\"pwned"; // attempted quote-breakout
    constStr.outputs.push_back(out);
    graph.nodes.push_back(constStr);
    graph.connections.push_back({1, 0, 2, 0});

    auto result = VisualScriptCompiler::Compile(graph);
    EXPECT_TRUE(result.success);
    // Pre-fix vulnerable output closed the string literal early with an
    // unescaped `"")` pair, letting `fireEvent(` follow as bare code — that
    // exact unescaped byte sequence must be absent post-fix.
    EXPECT_TRUE(result.angelScriptSource.find("\"\"); fireEvent(\"pwned") == std::string::npos);
    // Post-fix, the embedded quotes must be backslash-escaped so the whole
    // payload stays inside a single string literal.
    EXPECT_TRUE(result.angelScriptSource.find("\"\\\"); fireEvent(\\\"pwned\"") != std::string::npos);
}

TEST(VisualScriptCompiler_Security_SpawnEntityEscapesQuotes)
{
    using namespace Spark::Scripting;
    VisualScriptGraph graph;

    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    ScriptNode spawn;
    spawn.id = 2;
    spawn.type = ScriptNodeType::SpawnEntity;
    spawn.properties["name"] = "x\"); destroyEntity(0); //";
    ScriptPin execIn;
    execIn.kind = PinKind::Execution;
    spawn.inputs.push_back(execIn);
    ScriptPin uintOut;
    uintOut.kind = PinKind::Entity;
    spawn.outputs.push_back(uintOut);
    graph.nodes.push_back(spawn);
    graph.connections.push_back({1, 0, 2, 0});

    auto result = VisualScriptCompiler::Compile(graph);
    EXPECT_TRUE(result.success);

    // Pre-fix vulnerable output closed the string literal early with an
    // unescaped quote, letting `destroyEntity(0);` run as a real, second
    // statement — that exact unescaped byte sequence must be absent.
    EXPECT_TRUE(result.angelScriptSource.find("createEntity(\"x\"); destroyEntity(0);") == std::string::npos);
    // Post-fix, the payload must be present only inside a single escaped
    // string literal argument to createEntity().
    EXPECT_TRUE(result.angelScriptSource.find("createEntity(\"x\\\"); destroyEntity(0); //\")") != std::string::npos);
}

TEST(VisualScriptCompiler_Security_VariableNameSanitizedToIdentifier)
{
    using namespace Spark::Scripting;
    VisualScriptGraph graph;

    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    ScriptNode setVar;
    setVar.id = 2;
    setVar.type = ScriptNodeType::SetVariable;
    setVar.properties["name"] = "x; fireEvent(\"evil\"); //";
    ScriptPin execIn;
    execIn.kind = PinKind::Execution;
    ScriptPin dataIn;
    dataIn.kind = PinKind::Float;
    setVar.inputs.push_back(execIn);
    setVar.inputs.push_back(dataIn);
    graph.nodes.push_back(setVar);
    graph.connections.push_back({1, 0, 2, 0});

    auto result = VisualScriptCompiler::Compile(graph);
    EXPECT_TRUE(result.success);

    // The injected call must never appear as real, callable syntax in the
    // output — SanitizeIdentifier maps '(' to '_' too, so "fireEvent(" (with
    // a real paren) cannot survive anywhere in the emitted identifier.
    EXPECT_TRUE(result.angelScriptSource.find("fireEvent(\"evil\")") == std::string::npos);
    // The sanitized identifier (alnum/underscore run) must still be present,
    // proving the property was processed through SanitizeIdentifier rather
    // than silently dropped.
    EXPECT_TRUE(result.angelScriptSource.find("x__fireEvent__evil") != std::string::npos);
}

TEST(VisualScriptCompiler_Security_ClassNameSanitized)
{
    using namespace Spark::Scripting;
    VisualScriptGraph graph;
    graph.className = "Foo\nvoid Pwned(){ destroyEntity(0); }\nclass Bar";

    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    auto result = VisualScriptCompiler::Compile(graph);
    EXPECT_TRUE(result.success);

    // No embedded newline may reach the emitted "class <Name>\n{\n" token,
    // and the injected sibling method must not appear as real, callable code.
    EXPECT_TRUE(result.angelScriptSource.find("void Pwned()") == std::string::npos);
    EXPECT_TRUE(result.angelScriptSource.find("destroyEntity(0)") == std::string::npos);
    // The sanitized class name is emitted as a single alnum/underscore token
    // immediately after "class ".
    EXPECT_TRUE(result.angelScriptSource.find("class Foo_") != std::string::npos);
}

TEST(VisualScriptCompiler_Security_CleanInputUnaffected)
{
    // Regression/no-behavior-change check: ordinary ASCII-identifier-safe
    // input must compile exactly as before the fix (escaping/sanitizing are
    // no-ops on clean input).
    using namespace Spark::Scripting;
    VisualScriptGraph graph;
    graph.className = "PlayerHealth";

    ScriptNode start;
    start.id = 1;
    start.type = ScriptNodeType::OnStart;
    graph.nodes.push_back(start);

    ScriptNode playSound;
    playSound.id = 2;
    playSound.type = ScriptNodeType::PlaySound;
    playSound.properties["sound"] = "explosion.wav";
    ScriptPin execIn;
    execIn.kind = PinKind::Execution;
    playSound.inputs.push_back(execIn);
    graph.nodes.push_back(playSound);
    graph.connections.push_back({1, 0, 2, 0});

    auto result = VisualScriptCompiler::Compile(graph);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.angelScriptSource.find("class PlayerHealth") != std::string::npos);
    EXPECT_TRUE(result.angelScriptSource.find("playSound(selfEntity, \"explosion.wav\");") != std::string::npos);
}
