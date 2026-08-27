/**
 * @file ShaderGraphCompiler.cpp
 * @brief Node graph to HLSL compilation — topological traversal + code emission
 */

#include "ShaderGraphCompiler.h"
#include "../../Utils/ContainerUtils.h"
#include "../../Utils/LogMacros.h"
#include <algorithm>
#include <iomanip>
#include <queue>
#include <sstream>

namespace Spark::Graphics
{

    std::string ShaderGraphCompiler::VarName(uint32_t nodeID, uint32_t socketIndex)
    {
        return "node" + std::to_string(nodeID) + "_out" + std::to_string(socketIndex);
    }

    const ShaderConnection* ShaderGraphCompiler::FindConnectionToInput(const ShaderGraphInput& graph, uint32_t nodeID,
                                                                       uint32_t socketIndex)
    {
        for (const auto& conn : graph.connections)
        {
            if (conn.toNodeID == nodeID && conn.toSocketIndex == socketIndex)
                return &conn;
        }
        return nullptr;
    }

    std::string ShaderGraphCompiler::ResolveInput(const ShaderNode& node, uint32_t inputIndex,
                                                  const ShaderGraphInput& graph)
    {
        auto* conn = FindConnectionToInput(graph, node.id, inputIndex);
        if (conn)
            return VarName(conn->fromNodeID, conn->fromSocketIndex);

        // Use default value
        if (inputIndex < node.inputs.size())
        {
            const auto& def = node.inputs[inputIndex].defaultValue;
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(4);
            ss << "float4(" << def[0] << ", " << def[1] << ", " << def[2] << ", " << def[3] << ")";
            return ss.str();
        }
        return "float4(0, 0, 0, 0)";
    }

    std::vector<uint32_t> ShaderGraphCompiler::TopologicalSort(const ShaderGraphInput& graph, uint32_t outputNodeID)
    {
        // BFS backward from output node through connections
        std::unordered_set<uint32_t> visited;
        std::vector<uint32_t> order;
        std::queue<uint32_t> queue;
        queue.push(outputNodeID);
        visited.insert(outputNodeID);

        while (!queue.empty())
        {
            uint32_t current = queue.front();
            queue.pop();

            // Find all nodes that connect INTO this node
            for (const auto& conn : graph.connections)
            {
                if (conn.toNodeID == current && !Spark::ContainerUtils::Contains(visited, conn.fromNodeID))
                {
                    visited.insert(conn.fromNodeID);
                    queue.push(conn.fromNodeID);
                }
            }
            order.push_back(current);
        }

        // Reverse so dependencies come before dependents
        std::reverse(order.begin(), order.end());
        return order;
    }

    void ShaderGraphCompiler::EmitNode(const ShaderNode& node, const ShaderGraphInput& graph, std::ostringstream& code,
                                       int& textureSlot)
    {
        std::string out0 = VarName(node.id, 0);

        switch (node.type)
        {
        case ShaderNodeType::ConstantFloat:
            code << "    float " << out0 << " = " << node.inputs[0].defaultValue[0] << ";\n";
            break;

        case ShaderNodeType::ConstantVec2:
            code << "    float2 " << out0 << " = float2(" << node.inputs[0].defaultValue[0] << ", "
                 << node.inputs[0].defaultValue[1] << ");\n";
            break;

        case ShaderNodeType::ConstantVec3:
        case ShaderNodeType::ConstantColor:
            code << "    float3 " << out0 << " = float3(" << node.inputs[0].defaultValue[0] << ", "
                 << node.inputs[0].defaultValue[1] << ", " << node.inputs[0].defaultValue[2] << ");\n";
            break;

        case ShaderNodeType::ConstantVec4:
            code << "    float4 " << out0 << " = float4(" << node.inputs[0].defaultValue[0] << ", "
                 << node.inputs[0].defaultValue[1] << ", " << node.inputs[0].defaultValue[2] << ", "
                 << node.inputs[0].defaultValue[3] << ");\n";
            break;

        case ShaderNodeType::TextureSample:
        {
            std::string uv = ResolveInput(node, 0, graph);
            code << "    float4 " << out0 << " = tex" << textureSlot << ".Sample(linearSampler, " << uv << ".xy);\n";
            textureSlot++;
            break;
        }

        case ShaderNodeType::Time:
            code << "    float " << out0 << " = Time;\n";
            break;

        case ShaderNodeType::UVCoords:
            code << "    float2 " << out0 << " = input.TexCoord;\n";
            break;

        case ShaderNodeType::WorldPosition:
            code << "    float3 " << out0 << " = input.WorldPos;\n";
            break;

        case ShaderNodeType::WorldNormal:
            code << "    float3 " << out0 << " = input.Normal;\n";
            break;

        case ShaderNodeType::CameraVector:
            code << "    float3 " << out0 << " = normalize(CameraPos - input.WorldPos);\n";
            break;

        // Math operations
        case ShaderNodeType::Add:
            code << "    float4 " << out0 << " = " << ResolveInput(node, 0, graph) << " + "
                 << ResolveInput(node, 1, graph) << ";\n";
            break;

        case ShaderNodeType::Subtract:
            code << "    float4 " << out0 << " = " << ResolveInput(node, 0, graph) << " - "
                 << ResolveInput(node, 1, graph) << ";\n";
            break;

        case ShaderNodeType::Multiply:
            code << "    float4 " << out0 << " = " << ResolveInput(node, 0, graph) << " * "
                 << ResolveInput(node, 1, graph) << ";\n";
            break;

        case ShaderNodeType::Divide:
            code << "    float4 " << out0 << " = " << ResolveInput(node, 0, graph) << " / max("
                 << ResolveInput(node, 1, graph) << ", float4(0.001, 0.001, 0.001, 0.001));\n";
            break;

        case ShaderNodeType::DotProduct:
            code << "    float " << out0 << " = dot(" << ResolveInput(node, 0, graph) << ".xyz, "
                 << ResolveInput(node, 1, graph) << ".xyz);\n";
            break;

        case ShaderNodeType::CrossProduct:
            code << "    float3 " << out0 << " = cross(" << ResolveInput(node, 0, graph) << ".xyz, "
                 << ResolveInput(node, 1, graph) << ".xyz);\n";
            break;

        case ShaderNodeType::Normalize:
            code << "    float4 " << out0 << " = float4(normalize(" << ResolveInput(node, 0, graph) << ".xyz), 0);\n";
            break;

        case ShaderNodeType::Length:
            code << "    float " << out0 << " = length(" << ResolveInput(node, 0, graph) << ".xyz);\n";
            break;

        case ShaderNodeType::Power:
            code << "    float4 " << out0 << " = pow(abs(" << ResolveInput(node, 0, graph) << "), "
                 << ResolveInput(node, 1, graph) << ");\n";
            break;

        case ShaderNodeType::Sqrt:
            code << "    float4 " << out0 << " = sqrt(abs(" << ResolveInput(node, 0, graph) << "));\n";
            break;

        case ShaderNodeType::Sin:
            code << "    float4 " << out0 << " = sin(" << ResolveInput(node, 0, graph) << ");\n";
            break;

        case ShaderNodeType::Cos:
            code << "    float4 " << out0 << " = cos(" << ResolveInput(node, 0, graph) << ");\n";
            break;

        case ShaderNodeType::Lerp:
            code << "    float4 " << out0 << " = lerp(" << ResolveInput(node, 0, graph) << ", "
                 << ResolveInput(node, 1, graph) << ", " << ResolveInput(node, 2, graph) << ".x);\n";
            break;

        case ShaderNodeType::Clamp:
            code << "    float4 " << out0 << " = clamp(" << ResolveInput(node, 0, graph) << ", "
                 << ResolveInput(node, 1, graph) << ", " << ResolveInput(node, 2, graph) << ");\n";
            break;

        case ShaderNodeType::Saturate:
            code << "    float4 " << out0 << " = saturate(" << ResolveInput(node, 0, graph) << ");\n";
            break;

        case ShaderNodeType::Fresnel:
            code << "    float " << out0
                 << " = pow(1.0 - saturate(dot(input.Normal, normalize(CameraPos - "
                    "input.WorldPos))), "
                 << ResolveInput(node, 0, graph) << ".x);\n";
            break;

        case ShaderNodeType::SplitVector:
        {
            std::string in0 = ResolveInput(node, 0, graph);
            code << "    float " << VarName(node.id, 0) << " = " << in0 << ".x;\n";
            code << "    float " << VarName(node.id, 1) << " = " << in0 << ".y;\n";
            code << "    float " << VarName(node.id, 2) << " = " << in0 << ".z;\n";
            code << "    float " << VarName(node.id, 3) << " = " << in0 << ".w;\n";
            break;
        }

        case ShaderNodeType::CombineVector:
            code << "    float4 " << out0 << " = float4(" << ResolveInput(node, 0, graph) << ".x, "
                 << ResolveInput(node, 1, graph) << ".x, " << ResolveInput(node, 2, graph) << ".x, "
                 << ResolveInput(node, 3, graph) << ".x);\n";
            break;

        // Output nodes handled separately
        case ShaderNodeType::SurfaceOutput:
        case ShaderNodeType::UnlitOutput:
            break;

        default:
            code << "    // Unknown node type " << static_cast<uint32_t>(node.type) << "\n";
            code << "    float4 " << out0 << " = float4(1, 0, 1, 1); // Magenta = error\n";
            break;
        }
    }

    ShaderGraphOutput ShaderGraphCompiler::Compile(const ShaderGraphInput& graph)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Compiling shader graph '%s' (%zu nodes, %zu connections)",
                       graph.materialName.c_str(), graph.nodes.size(), graph.connections.size());
        ShaderGraphOutput output;

        // Find output node
        uint32_t outputNodeID = graph.surfaceOutputNodeID;
        bool isPBR = true;
        if (outputNodeID == 0)
        {
            outputNodeID = graph.unlitOutputNodeID;
            isPBR = false;
        }
        if (outputNodeID == 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "No output node found in shader graph '%s'",
                            graph.materialName.c_str());
            output.errors.push_back("No output node found in graph");
            return output;
        }

        // Find the output node data
        const ShaderNode* outputNode = nullptr;
        std::unordered_map<uint32_t, const ShaderNode*> nodeMap;
        for (const auto& node : graph.nodes)
        {
            nodeMap[node.id] = &node;
            if (node.id == outputNodeID)
                outputNode = &node;
        }
        if (!outputNode)
        {
            output.errors.push_back("Output node ID not found in node list");
            return output;
        }

        // Topological sort
        auto sortedIDs = TopologicalSort(graph, outputNodeID);

        // Count texture slots needed
        int textureSlot = 0;
        for (uint32_t id : sortedIDs)
        {
            auto it = nodeMap.find(id);
            if (it != nodeMap.end() && it->second->type == ShaderNodeType::TextureSample)
                output.textureSlotCount++;
        }

        // ================================================================
        // Vertex shader (standard PBR pass-through)
        // ================================================================
        output.vertexShader = R"(// Auto-generated vertex shader
cbuffer PerFrame : register(b0) { float4x4 ViewProjection; float3 CameraPos; float Time; };
cbuffer PerObject : register(b1) { float4x4 World; float4x4 WorldInvTranspose; };

struct VSInput { float3 Position : POSITION; float3 Normal : NORMAL; float2 TexCoord : TEXCOORD0; };
struct VSOutput { float4 Position : SV_POSITION; float3 WorldPos : TEXCOORD0; float3 Normal : TEXCOORD1; float2 TexCoord : TEXCOORD2; };

VSOutput main(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(float4(input.Position, 1.0), World);
    output.WorldPos = worldPos.xyz;
    output.Position = mul(worldPos, ViewProjection);
    output.Normal = normalize(mul(input.Normal, (float3x3)WorldInvTranspose));
    output.TexCoord = input.TexCoord;
    return output;
}
)";

        // ================================================================
        // Pixel shader (generated from graph)
        // ================================================================
        std::ostringstream ps;
        ps << "// Auto-generated pixel shader from material: " << graph.materialName << "\n\n";

        // Constant buffers
        ps << "cbuffer PerFrame : register(b0) { float4x4 ViewProjection; float3 CameraPos; float Time; };\n\n";

        // Texture declarations
        textureSlot = 0;
        for (uint32_t id : sortedIDs)
        {
            auto it = nodeMap.find(id);
            if (it != nodeMap.end() && it->second->type == ShaderNodeType::TextureSample)
            {
                ps << "Texture2D tex" << textureSlot << " : register(t" << textureSlot << ");\n";
                textureSlot++;
            }
        }
        if (textureSlot > 0)
            ps << "SamplerState linearSampler : register(s0);\n";
        ps << "\n";

        // Input struct
        ps << "struct PSInput { float4 Position : SV_POSITION; float3 WorldPos : TEXCOORD0; ";
        ps << "float3 Normal : TEXCOORD1; float2 TexCoord : TEXCOORD2; };\n\n";

        // Main function
        ps << "float4 main(PSInput input) : SV_TARGET\n{\n";

        // Emit nodes in topological order
        textureSlot = 0;
        for (uint32_t id : sortedIDs)
        {
            auto it = nodeMap.find(id);
            if (it != nodeMap.end() && id != outputNodeID)
                EmitNode(*it->second, graph, ps, textureSlot);
        }

        // Wire output
        if (isPBR)
        {
            std::string baseColor = ResolveInput(*outputNode, 0, graph) + ".xyz";
            std::string metallic = ResolveInput(*outputNode, 1, graph) + ".x";
            std::string roughness = ResolveInput(*outputNode, 2, graph) + ".x";

            ps << "\n    // PBR output\n";
            ps << "    float3 baseColor = " << baseColor << ";\n";
            ps << "    float metallic = " << metallic << ";\n";
            ps << "    float roughness = " << roughness << ";\n";
            ps << "    float3 N = normalize(input.Normal);\n";
            ps << "    float3 V = normalize(CameraPos - input.WorldPos);\n";
            ps << "    float3 L = normalize(float3(0.5, -0.7, 0.5));\n";
            ps << "    float3 H = normalize(V + L);\n";
            ps << "    float NdotL = max(dot(N, L), 0.0);\n";
            ps << "    float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);\n";
            ps << "    float3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);\n";
            ps << "    float3 kD = (1.0 - F) * (1.0 - metallic);\n";
            ps << "    float3 diffuse = kD * baseColor / 3.14159;\n";
            ps << "    float3 color = (diffuse + F * 0.25) * NdotL + baseColor * 0.03;\n";
            ps << "    return float4(color, 1.0);\n";
        }
        else
        {
            std::string color = ResolveInput(*outputNode, 0, graph) + ".xyz";
            std::string opacity = ResolveInput(*outputNode, 1, graph) + ".x";
            ps << "\n    return float4(" << color << ", " << opacity << ");\n";
        }

        ps << "}\n";
        output.pixelShader = ps.str();
        output.success = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "Shader graph compiled: %zu nodes processed, %d texture slots, %s output", sortedIDs.size(),
                       output.textureSlotCount, isPBR ? "PBR" : "Unlit");
        return output;
    }

} // namespace Spark::Graphics
