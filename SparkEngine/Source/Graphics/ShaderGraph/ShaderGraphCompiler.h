/**
 * @file ShaderGraphCompiler.h
 * @brief Compiles a material node graph into HLSL shader code (ezEngine-inspired)
 *
 * Traverses the MaterialGraph from output nodes back through connections,
 * resolves data types, and emits valid HLSL source code. Each node generates
 * a local variable; connections wire variables together.
 *
 * ## Algorithm
 * 1. Find the output node (Surface or Unlit)
 * 2. Topological sort: walk connections backward from output to inputs
 * 3. Emit variable declarations in topological order
 * 4. Wire output node inputs to PBR/unlit output struct
 *
 * @threadsafety Not thread-safe (single compilation at a time).
 */

#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark::Graphics
{

    // ========================================================================
    // Portable node type enum (mirrors SparkEditor::MaterialNodeType)
    // ========================================================================

    enum class ShaderNodeType : uint32_t
    {
        // Input nodes
        TextureSample = 0,
        ConstantFloat = 1,
        ConstantVec2 = 2,
        ConstantVec3 = 3,
        ConstantVec4 = 4,
        ConstantColor = 5,
        Time = 6,
        UVCoords = 7,
        WorldPosition = 8,
        WorldNormal = 9,
        CameraVector = 10,

        // Math nodes
        Add = 50,
        Subtract = 51,
        Multiply = 52,
        Divide = 53,
        DotProduct = 54,
        CrossProduct = 55,
        Normalize = 56,
        Length = 57,
        Distance = 58,
        Power = 59,
        Sqrt = 60,
        Sin = 61,
        Cos = 62,
        Tan = 63,
        Lerp = 64,
        Clamp = 65,
        Saturate = 66,

        // Utility nodes
        Fresnel = 100,
        Noise = 101,
        SplitVector = 105,
        CombineVector = 106,

        // Output nodes
        SurfaceOutput = 200,
        UnlitOutput = 201,
    };

    // ========================================================================
    // Compiler input structures (lightweight copies of editor graph data)
    // ========================================================================

    struct ShaderNodeInput
    {
        float defaultValue[4] = {0, 0, 0, 0};
        bool isConnected = false;
    };

    struct ShaderNode
    {
        uint32_t id = 0;
        ShaderNodeType type{};
        std::vector<ShaderNodeInput> inputs;
        std::unordered_map<std::string, std::string> properties;
    };

    struct ShaderConnection
    {
        uint32_t fromNodeID = 0;
        uint32_t fromSocketIndex = 0;
        uint32_t toNodeID = 0;
        uint32_t toSocketIndex = 0;
    };

    struct ShaderGraphInput
    {
        std::vector<ShaderNode> nodes;
        std::vector<ShaderConnection> connections;
        uint32_t surfaceOutputNodeID = 0;
        uint32_t unlitOutputNodeID = 0;
        std::string materialName = "Material";
    };

    struct ShaderGraphOutput
    {
        std::string vertexShader;
        std::string pixelShader;
        std::vector<std::string> errors;
        int textureSlotCount = 0;
        bool success = false;
    };

    // ========================================================================
    // Compiler
    // ========================================================================

    /**
     * @brief Compiles a node graph into HLSL vertex + pixel shader code.
     */
    class ShaderGraphCompiler
    {
      public:
        /**
         * @brief Compile a shader graph to HLSL.
         * @param graph  Input graph with nodes and connections.
         * @return Output with generated shaders or error messages.
         */
        static ShaderGraphOutput Compile(const ShaderGraphInput& graph);

      private:
        /// Generate a unique variable name for a node's output
        static std::string VarName(uint32_t nodeID, uint32_t socketIndex);

        /// Topological sort of nodes reachable from the output node
        static std::vector<uint32_t> TopologicalSort(const ShaderGraphInput& graph, uint32_t outputNodeID);

        /// Emit HLSL code for a single node
        static void EmitNode(const ShaderNode& node, const ShaderGraphInput& graph, std::ostringstream& code,
                             int& textureSlot);

        /// Get the HLSL expression for a node input (either connected variable or default value)
        static std::string ResolveInput(const ShaderNode& node, uint32_t inputIndex, const ShaderGraphInput& graph);

        /// Find the node connected to a specific input socket
        static const ShaderConnection* FindConnectionToInput(const ShaderGraphInput& graph, uint32_t nodeID,
                                                             uint32_t socketIndex);
    };

} // namespace Spark::Graphics
