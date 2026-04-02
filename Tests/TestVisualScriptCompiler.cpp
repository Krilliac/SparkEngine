// TestVisualScriptCompiler.cpp - Tests for visual script graph to AngelScript compilation
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <algorithm>
#include <cstdint>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
