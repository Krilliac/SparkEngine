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
