// TestShaderGraphCompiler.cpp - Tests for shader graph to HLSL compilation
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <queue>

namespace TestSGC
{

    enum class NodeType : uint32_t
    {
        ConstFloat = 1,
        ConstVec3 = 3,
        Add = 50,
        Multiply = 52,
        Lerp = 64,
        SurfaceOutput = 200,
    };

    struct NodeInput
    {
        float defaultValue[4] = {0, 0, 0, 0};
        bool isConnected = false;
    };

    struct Node
    {
        uint32_t id = 0;
        NodeType type{};
        std::vector<NodeInput> inputs;
    };

    struct Connection
    {
        uint32_t fromNodeID = 0;
        uint32_t fromSocketIndex = 0;
        uint32_t toNodeID = 0;
        uint32_t toSocketIndex = 0;
    };

    struct Graph
    {
        std::vector<Node> nodes;
        std::vector<Connection> connections;
        uint32_t outputNodeID = 0;
    };

    std::string VarName(uint32_t nodeID, uint32_t socketIndex)
    {
        return "node" + std::to_string(nodeID) + "_out" + std::to_string(socketIndex);
    }

    std::vector<uint32_t> TopologicalSort(const Graph& graph, uint32_t outputNodeID)
    {
        std::unordered_set<uint32_t> visited;
        std::vector<uint32_t> order;
        std::queue<uint32_t> q;
        q.push(outputNodeID);
        visited.insert(outputNodeID);

        while (!q.empty())
        {
            uint32_t current = q.front();
            q.pop();
            for (const auto& conn : graph.connections)
            {
                if (conn.toNodeID == current && visited.find(conn.fromNodeID) == visited.end())
                {
                    visited.insert(conn.fromNodeID);
                    q.push(conn.fromNodeID);
                }
            }
            order.push_back(current);
        }
        std::reverse(order.begin(), order.end());
        return order;
    }

    const Connection* FindConnectionToInput(const Graph& graph, uint32_t nodeID, uint32_t socketIndex)
    {
        for (const auto& conn : graph.connections)
        {
            if (conn.toNodeID == nodeID && conn.toSocketIndex == socketIndex)
                return &conn;
        }
        return nullptr;
    }

} // namespace TestSGC

using namespace TestSGC;

TEST(ShaderGraph_TopologicalSort_SingleNode)
{
    Graph graph;
    Node output;
    output.id = 1;
    output.type = NodeType::SurfaceOutput;
    graph.nodes.push_back(output);
    graph.outputNodeID = 1;

    auto order = TopologicalSort(graph, 1);
    EXPECT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 1u);
}

TEST(ShaderGraph_TopologicalSort_Chain)
{
    // ConstFloat(1) -> Add(2) -> SurfaceOutput(3)
    Graph graph;
    Node n1;
    n1.id = 1;
    n1.type = NodeType::ConstFloat;
    graph.nodes.push_back(n1);

    Node n2;
    n2.id = 2;
    n2.type = NodeType::Add;
    graph.nodes.push_back(n2);

    Node n3;
    n3.id = 3;
    n3.type = NodeType::SurfaceOutput;
    graph.nodes.push_back(n3);

    graph.connections.push_back({1, 0, 2, 0}); // n1 -> n2
    graph.connections.push_back({2, 0, 3, 0}); // n2 -> n3
    graph.outputNodeID = 3;

    auto order = TopologicalSort(graph, 3);
    EXPECT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1u); // ConstFloat first
    EXPECT_EQ(order[1], 2u); // Add second
    EXPECT_EQ(order[2], 3u); // Output last
}

TEST(ShaderGraph_FindConnection)
{
    Graph graph;
    graph.connections.push_back({10, 0, 20, 1}); // node10.out0 -> node20.in1

    auto* conn = FindConnectionToInput(graph, 20, 1);
    EXPECT_TRUE(conn != nullptr);
    EXPECT_EQ(conn->fromNodeID, 10u);
    EXPECT_EQ(conn->fromSocketIndex, 0u);

    auto* miss = FindConnectionToInput(graph, 20, 0);
    EXPECT_TRUE(miss == nullptr);
}

TEST(ShaderGraph_VarName)
{
    EXPECT_EQ(VarName(5, 0), std::string("node5_out0"));
    EXPECT_EQ(VarName(42, 2), std::string("node42_out2"));
}

TEST(ShaderGraph_TopologicalSort_Diamond)
{
    // n1 -> n3
    // n2 -> n3 -> n4 (output)
    Graph graph;
    for (uint32_t i = 1; i <= 4; ++i)
    {
        Node n;
        n.id = i;
        graph.nodes.push_back(n);
    }
    graph.connections.push_back({1, 0, 3, 0});
    graph.connections.push_back({2, 0, 3, 1});
    graph.connections.push_back({3, 0, 4, 0});
    graph.outputNodeID = 4;

    auto order = TopologicalSort(graph, 4);
    EXPECT_EQ(order.size(), 4u);

    // n1 and n2 must come before n3, n3 before n4
    auto posOf = [&](uint32_t id) -> size_t
    {
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == id)
                return i;
        return 999;
    };
    EXPECT_TRUE(posOf(1) < posOf(3));
    EXPECT_TRUE(posOf(2) < posOf(3));
    EXPECT_TRUE(posOf(3) < posOf(4));
}

// ============================================================================
// Additional standalone helpers for code generation, type checking, cycles
// ============================================================================

namespace TestSGC
{

    enum class PinType : uint32_t
    {
        Float = 1,
        Float2 = 2,
        Float3 = 3,
        Float4 = 4,
    };

    struct PinDescriptor
    {
        std::string name;
        PinType type = PinType::Float;
    };

    struct TypedNode
    {
        uint32_t id = 0;
        NodeType type{};
        std::vector<PinDescriptor> inputPins;
        std::vector<PinDescriptor> outputPins;
    };

    struct TypeCheckResult
    {
        bool valid = true;
        std::string error;
    };

    TypeCheckResult CheckConnectionTypes(const TypedNode& from, uint32_t fromSocket, const TypedNode& to,
                                         uint32_t toSocket)
    {
        if (fromSocket >= from.outputPins.size())
            return {false, "Invalid output socket index"};
        if (toSocket >= to.inputPins.size())
            return {false, "Invalid input socket index"};

        if (from.outputPins[fromSocket].type != to.inputPins[toSocket].type)
        {
            return {false, "Type mismatch: output is " +
                               std::to_string(static_cast<uint32_t>(from.outputPins[fromSocket].type)) +
                               " but input expects " +
                               std::to_string(static_cast<uint32_t>(to.inputPins[toSocket].type))};
        }
        return {true, ""};
    }

    bool DetectCycle(const Graph& graph)
    {
        std::unordered_map<uint32_t, std::vector<uint32_t>> adjacency;
        for (const auto& node : graph.nodes)
            adjacency[node.id]; // ensure all nodes present
        for (const auto& conn : graph.connections)
            adjacency[conn.fromNodeID].push_back(conn.toNodeID);

        // 0 = unvisited, 1 = in progress, 2 = done
        std::unordered_map<uint32_t, int> state;
        for (const auto& [id, _] : adjacency)
            state[id] = 0;

        std::function<bool(uint32_t)> dfs = [&](uint32_t id) -> bool
        {
            state[id] = 1;
            for (uint32_t neighbor : adjacency[id])
            {
                if (state[neighbor] == 1)
                    return true; // cycle found
                if (state[neighbor] == 0 && dfs(neighbor))
                    return true;
            }
            state[id] = 2;
            return false;
        };

        for (const auto& [id, _] : adjacency)
        {
            if (state[id] == 0 && dfs(id))
                return true;
        }
        return false;
    }

    std::string GenerateHLSL(const Graph& graph)
    {
        std::ostringstream out;
        auto order = TopologicalSort(graph, graph.outputNodeID);

        for (uint32_t nodeID : order)
        {
            const Node* node = nullptr;
            for (const auto& n : graph.nodes)
            {
                if (n.id == nodeID)
                {
                    node = &n;
                    break;
                }
            }
            if (!node)
                continue;

            switch (node->type)
            {
            case NodeType::ConstFloat:
                out << "float " << VarName(nodeID, 0) << " = " << node->inputs[0].defaultValue[0] << ";\n";
                break;
            case NodeType::ConstVec3:
                out << "float3 " << VarName(nodeID, 0) << " = float3(" << node->inputs[0].defaultValue[0] << ", "
                    << node->inputs[0].defaultValue[1] << ", " << node->inputs[0].defaultValue[2] << ");\n";
                break;
            case NodeType::Multiply:
            {
                auto* connA = FindConnectionToInput(graph, nodeID, 0);
                auto* connB = FindConnectionToInput(graph, nodeID, 1);
                std::string a = connA ? VarName(connA->fromNodeID, connA->fromSocketIndex) : "0";
                std::string b = connB ? VarName(connB->fromNodeID, connB->fromSocketIndex) : "0";
                out << "float " << VarName(nodeID, 0) << " = " << a << " * " << b << ";\n";
                break;
            }
            case NodeType::SurfaceOutput:
            {
                auto* conn = FindConnectionToInput(graph, nodeID, 0);
                std::string src = conn ? VarName(conn->fromNodeID, conn->fromSocketIndex) : "0";
                out << "output.color = " << src << ";\n";
                break;
            }
            default:
                break;
            }
        }
        return out.str();
    }

} // namespace TestSGC

// ============================================================================
// HLSL Code Generation
// ============================================================================

TEST(ShaderGraph_HLSLCodeGeneration)
{
    // Build a simple graph: ConstFloat(1) * ConstFloat(2) -> SurfaceOutput(3)
    Graph graph;

    Node constA;
    constA.id = 1;
    constA.type = NodeType::ConstFloat;
    constA.inputs.push_back({{1.0f, 0, 0, 0}, false});
    graph.nodes.push_back(constA);

    Node constB;
    constB.id = 2;
    constB.type = NodeType::ConstFloat;
    constB.inputs.push_back({{0.5f, 0, 0, 0}, false});
    graph.nodes.push_back(constB);

    Node mul;
    mul.id = 3;
    mul.type = NodeType::Multiply;
    graph.nodes.push_back(mul);

    Node output;
    output.id = 4;
    output.type = NodeType::SurfaceOutput;
    graph.nodes.push_back(output);

    graph.connections.push_back({1, 0, 3, 0}); // constA -> mul input 0
    graph.connections.push_back({2, 0, 3, 1}); // constB -> mul input 1
    graph.connections.push_back({3, 0, 4, 0}); // mul -> output
    graph.outputNodeID = 4;

    std::string hlsl = TestSGC::GenerateHLSL(graph);

    // Verify generated code contains expected tokens
    EXPECT_TRUE(hlsl.find("float node1_out0") != std::string::npos);
    EXPECT_TRUE(hlsl.find("float node2_out0") != std::string::npos);
    EXPECT_TRUE(hlsl.find("node1_out0 * node2_out0") != std::string::npos);
    EXPECT_TRUE(hlsl.find("output.color") != std::string::npos);
}

// ============================================================================
// Node Connection Type Check
// ============================================================================

TEST(ShaderGraph_NodeConnectionTypeCheck)
{
    // Node A outputs a float
    TestSGC::TypedNode nodeA;
    nodeA.id = 1;
    nodeA.outputPins.push_back({"value", TestSGC::PinType::Float});

    // Node B expects a float3 input
    TestSGC::TypedNode nodeB;
    nodeB.id = 2;
    nodeB.inputPins.push_back({"color", TestSGC::PinType::Float3});

    // Connecting float output to float3 input should fail
    auto result = TestSGC::CheckConnectionTypes(nodeA, 0, nodeB, 0);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.error.find("mismatch") != std::string::npos);

    // Matching types should succeed
    TestSGC::TypedNode nodeC;
    nodeC.id = 3;
    nodeC.inputPins.push_back({"intensity", TestSGC::PinType::Float});

    auto validResult = TestSGC::CheckConnectionTypes(nodeA, 0, nodeC, 0);
    EXPECT_TRUE(validResult.valid);
}

// ============================================================================
// Cycle Detection
// ============================================================================

TEST(ShaderGraph_CycleDetection)
{
    // Create a graph with a circular dependency: n1 -> n2 -> n3 -> n1
    Graph graph;

    Node n1;
    n1.id = 1;
    n1.type = NodeType::Add;
    graph.nodes.push_back(n1);

    Node n2;
    n2.id = 2;
    n2.type = NodeType::Multiply;
    graph.nodes.push_back(n2);

    Node n3;
    n3.id = 3;
    n3.type = NodeType::Add;
    graph.nodes.push_back(n3);

    graph.connections.push_back({1, 0, 2, 0}); // n1 -> n2
    graph.connections.push_back({2, 0, 3, 0}); // n2 -> n3
    graph.connections.push_back({3, 0, 1, 0}); // n3 -> n1 (cycle!)
    graph.outputNodeID = 3;

    EXPECT_TRUE(TestSGC::DetectCycle(graph));

    // Acyclic graph should return false
    Graph acyclic;
    Node a1;
    a1.id = 1;
    acyclic.nodes.push_back(a1);
    Node a2;
    a2.id = 2;
    acyclic.nodes.push_back(a2);
    acyclic.connections.push_back({1, 0, 2, 0});
    acyclic.outputNodeID = 2;

    EXPECT_FALSE(TestSGC::DetectCycle(acyclic));
}
