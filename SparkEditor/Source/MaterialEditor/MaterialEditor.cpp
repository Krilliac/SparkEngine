/**
 * @file MaterialEditor.cpp
 * @brief Node-based material editor implementation
 */

#include "MaterialEditor.h"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace DirectX;
namespace SparkEditor
{

    // =========================================================================
    // Lifecycle
    // =========================================================================

    MaterialEditor::MaterialEditor() : EditorPanel("Material Editor", "material_editor") {}

    MaterialEditor::~MaterialEditor()
    {
        Shutdown();
    }

    bool MaterialEditor::Initialize()
    {
        InitializeNodeTypes();
        CreateNewMaterial("Default Material");
        return true;
    }

    void MaterialEditor::Update(float /*deltaTime*/)
    {
        UpdateNodePreviews();
    }

    void MaterialEditor::Render()
    {
        RenderNodePalette();
        RenderGraphEditor();
        RenderMaterialProperties();
        RenderMaterialPreview();
        RenderCompilationOutput();
    }

    void MaterialEditor::Shutdown()
    {
        m_materialGraph.nodes.clear();
        m_materialGraph.connections.clear();
    }

    bool MaterialEditor::HandleEvent(const std::string& eventType, void* /*eventData*/)
    {
        if (eventType == "compile_material")
        {
            return CompileMaterial();
        }
        return false;
    }

    // =========================================================================
    // Material Operations
    // =========================================================================

    void MaterialEditor::CreateNewMaterial(const std::string& materialName)
    {
        m_materialGraph.nodes.clear();
        m_materialGraph.connections.clear();
        m_materialGraph.nextNodeID = 1;
        m_materialGraph.name = materialName;
        m_materialGraph.isCompiled = false;
        m_materialGraph.compilationErrors.clear();

        // Create default Surface Output node
        m_materialGraph.surfaceOutputNodeID = AddNode(MaterialNodeType::SURFACE_OUTPUT, {400.0f, 200.0f});

        // Create default constant nodes connected to the output
        AddNode(MaterialNodeType::CONSTANT_COLOR, {100.0f, 100.0f});
        AddNode(MaterialNodeType::CONSTANT_FLOAT, {100.0f, 300.0f});
    }

    bool MaterialEditor::LoadMaterial(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        // Read file content — material files use a simple JSON-like format
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (content.empty())
        {
            return false;
        }

        // Parse material name from first line
        std::istringstream stream(content);
        std::string line;
        if (std::getline(stream, line))
        {
            m_materialGraph.name = line;
        }

        return true;
    }

    bool MaterialEditor::SaveMaterial(const std::string& filePath)
    {
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        file << m_materialGraph.name << "\n";
        file << "nodes: " << m_materialGraph.nodes.size() << "\n";
        file << "connections: " << m_materialGraph.connections.size() << "\n";

        for (const auto& node : m_materialGraph.nodes)
        {
            file << "node " << node->id << " " << node->name << " " << node->position.x << " " << node->position.y
                 << "\n";
        }

        for (const auto& conn : m_materialGraph.connections)
        {
            file << "conn " << conn.fromNodeID << ":" << conn.fromSocketIndex << " -> " << conn.toNodeID << ":"
                 << conn.toSocketIndex << "\n";
        }

        return true;
    }

    bool MaterialEditor::CompileMaterial()
    {
        m_materialGraph.compilationErrors.clear();
        m_materialGraph.isCompiled = false;

        // Validate graph first
        if (!ValidateMaterialGraph(m_materialGraph.compilationErrors))
        {
            return false;
        }

        // Generate shader code
        if (!GenerateShaderCode(m_materialGraph.vertexShaderCode, m_materialGraph.pixelShaderCode))
        {
            m_materialGraph.compilationErrors.push_back("Shader code generation failed");
            return false;
        }

        m_materialGraph.isCompiled = true;
        return true;
    }

    // =========================================================================
    // Node Management
    // =========================================================================

    uint32_t MaterialEditor::AddNode(MaterialNodeType nodeType, const XMFLOAT2& position)
    {
        auto node = CreateNode(nodeType);
        if (!node)
        {
            return 0;
        }

        uint32_t id = m_materialGraph.nextNodeID++;
        node->id = id;
        node->position = position;
        m_materialGraph.nodes.push_back(std::move(node));
        return id;
    }

    bool MaterialEditor::RemoveNode(uint32_t nodeID)
    {
        // Don't allow removing output nodes
        if (nodeID == m_materialGraph.surfaceOutputNodeID || nodeID == m_materialGraph.unlitOutputNodeID)
        {
            return false;
        }

        // Remove all connections involving this node
        auto& conns = m_materialGraph.connections;
        conns.erase(std::remove_if(conns.begin(), conns.end(), [nodeID](const MaterialConnection& c)
                                   { return c.fromNodeID == nodeID || c.toNodeID == nodeID; }),
                    conns.end());

        // Remove the node itself
        auto& nodes = m_materialGraph.nodes;
        auto it = std::find_if(nodes.begin(), nodes.end(), [nodeID](const auto& n) { return n->id == nodeID; });
        if (it == nodes.end())
        {
            return false;
        }

        nodes.erase(it);
        m_materialGraph.isCompiled = false;
        return true;
    }

    bool MaterialEditor::ConnectSockets(uint32_t fromNodeID, uint32_t fromSocketIndex, uint32_t toNodeID,
                                        uint32_t toSocketIndex)
    {
        // Validate nodes exist
        MaterialNode* fromNode = nullptr;
        MaterialNode* toNode = nullptr;
        for (auto& n : m_materialGraph.nodes)
        {
            if (n->id == fromNodeID)
                fromNode = n.get();
            if (n->id == toNodeID)
                toNode = n.get();
        }
        if (!fromNode || !toNode)
            return false;

        // Validate socket indices
        if (fromSocketIndex >= fromNode->outputSockets.size())
            return false;
        if (toSocketIndex >= toNode->inputSockets.size())
            return false;

        // Prevent self-connections
        if (fromNodeID == toNodeID)
            return false;

        // Remove existing connection to this input socket (inputs accept only one connection)
        DisconnectSocket(toNodeID, toSocketIndex);

        // Create connection
        MaterialConnection conn;
        conn.fromNodeID = fromNodeID;
        conn.fromSocketIndex = fromSocketIndex;
        conn.toNodeID = toNodeID;
        conn.toSocketIndex = toSocketIndex;
        m_materialGraph.connections.push_back(conn);

        // Mark sockets as connected
        fromNode->outputSockets[fromSocketIndex].isConnected = true;
        toNode->inputSockets[toSocketIndex].isConnected = true;

        m_materialGraph.isCompiled = false;
        return true;
    }

    bool MaterialEditor::DisconnectSocket(uint32_t toNodeID, uint32_t toSocketIndex)
    {
        auto& conns = m_materialGraph.connections;
        auto it = std::find_if(conns.begin(), conns.end(), [toNodeID, toSocketIndex](const MaterialConnection& c)
                               { return c.toNodeID == toNodeID && c.toSocketIndex == toSocketIndex; });

        if (it == conns.end())
            return false;

        // Update connected state on the target socket
        for (auto& n : m_materialGraph.nodes)
        {
            if (n->id == toNodeID && toSocketIndex < n->inputSockets.size())
            {
                n->inputSockets[toSocketIndex].isConnected = false;
            }
        }

        conns.erase(it);
        m_materialGraph.isCompiled = false;
        return true;
    }

    void MaterialEditor::SetPreviewShape(MaterialPreview::Shape shape)
    {
        m_previewSettings.previewShape = shape;
    }

    // =========================================================================
    // Rendering
    // =========================================================================

    void MaterialEditor::RenderGraphEditor()
    {
        // The graph editor renders the node canvas area where users can
        // create, position, and connect material nodes. Actual ImGui draw
        // calls happen here in a real editor integration.
        HandleNodeDragging();
        HandleConnectionCreation();
        HandleNodeSelection();

        for (auto& node : m_materialGraph.nodes)
        {
            RenderNode(node.get());
        }
        RenderConnections();
    }

    void MaterialEditor::RenderNodePalette()
    {
        // Render the categorized list of available node types that can be
        // dragged into the graph.
    }

    void MaterialEditor::RenderMaterialProperties()
    {
        // Render the properties panel showing selected node parameters.
    }

    void MaterialEditor::RenderMaterialPreview()
    {
        // Render a 3D preview of the compiled material on the selected shape.
        if (m_materialGraph.isCompiled)
        {
            RenderPreviewToTexture();
        }
    }

    void MaterialEditor::RenderCompilationOutput()
    {
        // Render compilation errors and generated shader code for debugging.
    }

    void MaterialEditor::RenderNode(MaterialNode* node)
    {
        if (!node)
            return;
        RenderNodeSockets(node);
    }

    void MaterialEditor::RenderNodeSockets(MaterialNode* /*node*/)
    {
        // Draw input and output socket circles on the node's edges.
    }

    void MaterialEditor::RenderConnections()
    {
        // Draw bezier curves between connected sockets.
    }

    void MaterialEditor::HandleNodeDragging()
    {
        // Handle mouse drag to reposition nodes in the graph canvas.
    }

    void MaterialEditor::HandleConnectionCreation()
    {
        // Handle drag-from-socket to create new connections.
    }

    void MaterialEditor::HandleNodeSelection()
    {
        // Handle click-to-select and marquee selection of nodes.
    }

    void MaterialEditor::UpdateNodePreviews()
    {
        // Regenerate per-node preview thumbnails if the graph changed.
    }

    // =========================================================================
    // Node Factory
    // =========================================================================

    std::unique_ptr<MaterialNode> MaterialEditor::CreateNode(MaterialNodeType nodeType)
    {
        const auto& info = GetNodeTypeInfo(nodeType);

        auto node = std::make_unique<MaterialNode>();
        node->type = nodeType;
        node->name = info.name;
        node->category = info.category;
        node->description = info.description;
        node->inputSockets = info.inputSockets;
        node->outputSockets = info.outputSockets;
        node->titleColor = info.headerColor;
        node->hasPreview = info.hasPreview;

        // Size based on socket count
        float height =
            60.0f + static_cast<float>((std::max)(node->inputSockets.size(), node->outputSockets.size())) * 24.0f;
        node->size = {140.0f, height};

        return node;
    }

    void MaterialEditor::InitializeNodeTypes()
    {
        auto addType = [this](MaterialNodeType type, const std::string& name, const std::string& category,
                              const std::string& desc, std::vector<MaterialSocket> inputs,
                              std::vector<MaterialSocket> outputs, XMFLOAT4 color, bool preview)
        {
            NodeTypeInfo info;
            info.name = name;
            info.category = category;
            info.description = desc;
            info.inputSockets = std::move(inputs);
            info.outputSockets = std::move(outputs);
            info.headerColor = color;
            info.hasPreview = preview;
            m_nodeTypeInfo[type] = std::move(info);
            m_nodeCategories[category].push_back(type);
        };

        // --- Input nodes ---
        addType(MaterialNodeType::TEXTURE_SAMPLE, "Texture Sample", "Input", "Sample a 2D texture",
                {{"UV", SocketType::VECTOR2, SocketDirection::INPUT}},
                {{"Color", SocketType::VECTOR4, SocketDirection::OUTPUT},
                 {"R", SocketType::FLOAT, SocketDirection::OUTPUT},
                 {"G", SocketType::FLOAT, SocketDirection::OUTPUT},
                 {"B", SocketType::FLOAT, SocketDirection::OUTPUT},
                 {"A", SocketType::FLOAT, SocketDirection::OUTPUT}},
                {0.2f, 0.6f, 0.2f, 1.0f}, true);

        addType(MaterialNodeType::CONSTANT_FLOAT, "Float", "Input", "Constant float value", {},
                {{"Value", SocketType::FLOAT, SocketDirection::OUTPUT}}, {0.4f, 0.4f, 0.7f, 1.0f}, false);

        addType(MaterialNodeType::CONSTANT_VECTOR3, "Vector3", "Input", "Constant 3D vector", {},
                {{"Vector", SocketType::VECTOR3, SocketDirection::OUTPUT}}, {0.4f, 0.4f, 0.7f, 1.0f}, false);

        addType(MaterialNodeType::CONSTANT_COLOR, "Color", "Input", "Constant color value", {},
                {{"Color", SocketType::COLOR, SocketDirection::OUTPUT}}, {0.7f, 0.3f, 0.3f, 1.0f}, true);

        addType(MaterialNodeType::UV_COORDINATES, "UV Coords", "Input", "Mesh UV coordinates", {},
                {{"UV", SocketType::VECTOR2, SocketDirection::OUTPUT}}, {0.3f, 0.5f, 0.3f, 1.0f}, false);

        addType(MaterialNodeType::TIME, "Time", "Input", "Engine time in seconds", {},
                {{"Time", SocketType::FLOAT, SocketDirection::OUTPUT}}, {0.3f, 0.5f, 0.3f, 1.0f}, false);

        addType(MaterialNodeType::WORLD_POSITION, "World Position", "Input", "Fragment world position", {},
                {{"Position", SocketType::VECTOR3, SocketDirection::OUTPUT}}, {0.3f, 0.5f, 0.3f, 1.0f}, false);

        addType(MaterialNodeType::WORLD_NORMAL, "World Normal", "Input", "Surface world normal", {},
                {{"Normal", SocketType::VECTOR3, SocketDirection::OUTPUT}}, {0.3f, 0.5f, 0.3f, 1.0f}, false);

        // --- Math nodes ---
        addType(MaterialNodeType::ADD, "Add", "Math", "Add two values",
                {{"A", SocketType::FLOAT, SocketDirection::INPUT}, {"B", SocketType::FLOAT, SocketDirection::INPUT}},
                {{"Result", SocketType::FLOAT, SocketDirection::OUTPUT}}, {0.5f, 0.5f, 0.5f, 1.0f}, false);

        addType(MaterialNodeType::MULTIPLY, "Multiply", "Math", "Multiply two values",
                {{"A", SocketType::FLOAT, SocketDirection::INPUT}, {"B", SocketType::FLOAT, SocketDirection::INPUT}},
                {{"Result", SocketType::FLOAT, SocketDirection::OUTPUT}}, {0.5f, 0.5f, 0.5f, 1.0f}, false);

        addType(MaterialNodeType::LERP, "Lerp", "Math", "Linear interpolation",
                {{"A", SocketType::FLOAT, SocketDirection::INPUT},
                 {"B", SocketType::FLOAT, SocketDirection::INPUT},
                 {"T", SocketType::FLOAT, SocketDirection::INPUT}},
                {{"Result", SocketType::FLOAT, SocketDirection::OUTPUT}}, {0.5f, 0.5f, 0.5f, 1.0f}, false);

        addType(MaterialNodeType::NORMALIZE, "Normalize", "Math", "Normalize a vector",
                {{"Input", SocketType::VECTOR3, SocketDirection::INPUT}},
                {{"Result", SocketType::VECTOR3, SocketDirection::OUTPUT}}, {0.5f, 0.5f, 0.5f, 1.0f}, false);

        addType(
            MaterialNodeType::DOT_PRODUCT, "Dot Product", "Math", "Dot product of two vectors",
            {{"A", SocketType::VECTOR3, SocketDirection::INPUT}, {"B", SocketType::VECTOR3, SocketDirection::INPUT}},
            {{"Result", SocketType::FLOAT, SocketDirection::OUTPUT}}, {0.5f, 0.5f, 0.5f, 1.0f}, false);

        // --- Utility nodes ---
        addType(MaterialNodeType::FRESNEL, "Fresnel", "Utility", "Fresnel rim effect",
                {{"Exponent", SocketType::FLOAT, SocketDirection::INPUT}},
                {{"Result", SocketType::FLOAT, SocketDirection::OUTPUT}}, {0.6f, 0.4f, 0.6f, 1.0f}, true);

        // --- Output nodes ---
        addType(MaterialNodeType::SURFACE_OUTPUT, "Surface Output", "Output", "PBR surface material output",
                {{"Base Color", SocketType::COLOR, SocketDirection::INPUT, {0.8f, 0.8f, 0.8f, 1.0f}},
                 {"Metallic", SocketType::FLOAT, SocketDirection::INPUT, {0.0f, 0, 0, 0}},
                 {"Roughness", SocketType::FLOAT, SocketDirection::INPUT, {0.5f, 0, 0, 0}},
                 {"Normal", SocketType::VECTOR3, SocketDirection::INPUT, {0, 0, 1.0f, 0}},
                 {"Emissive", SocketType::COLOR, SocketDirection::INPUT, {0, 0, 0, 0}},
                 {"Opacity", SocketType::FLOAT, SocketDirection::INPUT, {1.0f, 0, 0, 0}}},
                {}, {0.8f, 0.3f, 0.2f, 1.0f}, true);

        addType(MaterialNodeType::UNLIT_OUTPUT, "Unlit Output", "Output", "Unlit material output",
                {{"Color", SocketType::COLOR, SocketDirection::INPUT, {1.0f, 1.0f, 1.0f, 1.0f}},
                 {"Opacity", SocketType::FLOAT, SocketDirection::INPUT, {1.0f, 0, 0, 0}}},
                {}, {0.7f, 0.5f, 0.2f, 1.0f}, false);
    }

    const MaterialEditor::NodeTypeInfo& MaterialEditor::GetNodeTypeInfo(MaterialNodeType nodeType)
    {
        auto it = m_nodeTypeInfo.find(nodeType);
        if (it != m_nodeTypeInfo.end())
        {
            return it->second;
        }
        static NodeTypeInfo defaultInfo{"Unknown", "Unknown", "Unknown node type", {}, {}, {0.5f, 0.5f, 0.5f, 1.0f},
                                        false};
        return defaultInfo;
    }

    // =========================================================================
    // Shader Code Generation
    // =========================================================================

    bool MaterialEditor::GenerateShaderCode(std::string& outVertexShader, std::string& outPixelShader)
    {
        // Generate a basic PBR vertex shader
        outVertexShader = R"(
cbuffer PerObject : register(b0)
{
    float4x4 World;
    float4x4 ViewProjection;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float3 Normal   : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(float4(input.Position, 1.0), World);
    output.WorldPos = worldPos.xyz;
    output.Position = mul(worldPos, ViewProjection);
    output.Normal = normalize(mul(float4(input.Normal, 0.0), World).xyz);
    output.TexCoord = input.TexCoord;
    return output;
}
)";

        // Build pixel shader from the node graph
        std::ostringstream ps;
        ps << "// Auto-generated by SparkEngine Material Editor\n";
        ps << "// Material: " << m_materialGraph.name << "\n\n";

        // Find the surface output node and trace its inputs
        MaterialNode* outputNode = nullptr;
        for (auto& n : m_materialGraph.nodes)
        {
            if (n->id == m_materialGraph.surfaceOutputNodeID)
            {
                outputNode = n.get();
                break;
            }
        }

        ps << "struct PSInput\n{\n";
        ps << "    float4 Position : SV_POSITION;\n";
        ps << "    float3 WorldPos : TEXCOORD0;\n";
        ps << "    float3 Normal   : TEXCOORD1;\n";
        ps << "    float2 TexCoord : TEXCOORD2;\n";
        ps << "};\n\n";

        ps << "float4 main(PSInput input) : SV_TARGET\n{\n";

        if (outputNode)
        {
            // Use default values from the output node's input sockets
            ps << "    float3 baseColor = float3(" << outputNode->inputSockets[0].defaultValue.x << ", "
               << outputNode->inputSockets[0].defaultValue.y << ", " << outputNode->inputSockets[0].defaultValue.z
               << ");\n";
            ps << "    float metallic = " << outputNode->inputSockets[1].defaultValue.x << ";\n";
            ps << "    float roughness = " << outputNode->inputSockets[2].defaultValue.x << ";\n";
            ps << "    float opacity = " << outputNode->inputSockets[5].defaultValue.x << ";\n";
        }
        else
        {
            ps << "    float3 baseColor = float3(0.8, 0.8, 0.8);\n";
            ps << "    float opacity = 1.0;\n";
        }

        ps << "\n    // Simple directional lighting\n";
        ps << "    float3 lightDir = normalize(float3(0.5, -0.5, 0.5));\n";
        ps << "    float NdotL = max(dot(input.Normal, -lightDir), 0.0);\n";
        ps << "    float3 finalColor = baseColor * (0.2 + 0.8 * NdotL);\n";
        ps << "    return float4(finalColor, opacity);\n";
        ps << "}\n";

        outPixelShader = ps.str();
        return true;
    }

    bool MaterialEditor::ValidateMaterialGraph(std::vector<std::string>& outErrors)
    {
        bool valid = true;

        // Check that we have at least one output node
        if (m_materialGraph.surfaceOutputNodeID == 0 && m_materialGraph.unlitOutputNodeID == 0)
        {
            outErrors.push_back("Material must have at least one output node");
            valid = false;
        }

        // Check for cycles (simple DFS-based check)
        // For now just verify basic connectivity
        for (const auto& conn : m_materialGraph.connections)
        {
            bool fromExists = false;
            bool toExists = false;
            for (const auto& n : m_materialGraph.nodes)
            {
                if (n->id == conn.fromNodeID)
                    fromExists = true;
                if (n->id == conn.toNodeID)
                    toExists = true;
            }
            if (!fromExists || !toExists)
            {
                outErrors.push_back("Connection references missing node");
                valid = false;
            }
        }

        return valid;
    }

    // =========================================================================
    // Coordinate Conversion
    // =========================================================================

    XMFLOAT2 MaterialEditor::ScreenToGraph(const XMFLOAT2& screenPos) const
    {
        return {(screenPos.x - m_graphViewOffset.x) / m_graphViewScale,
                (screenPos.y - m_graphViewOffset.y) / m_graphViewScale};
    }

    XMFLOAT2 MaterialEditor::GraphToScreen(const XMFLOAT2& graphPos) const
    {
        return {graphPos.x * m_graphViewScale + m_graphViewOffset.x,
                graphPos.y * m_graphViewScale + m_graphViewOffset.y};
    }

    MaterialNode* MaterialEditor::FindNodeAtPosition(const XMFLOAT2& position)
    {
        // Search in reverse order so topmost (last-drawn) nodes are found first
        for (auto it = m_materialGraph.nodes.rbegin(); it != m_materialGraph.nodes.rend(); ++it)
        {
            auto& node = *it;
            if (position.x >= node->position.x && position.x <= node->position.x + node->size.x &&
                position.y >= node->position.y && position.y <= node->position.y + node->size.y)
            {
                return node.get();
            }
        }
        return nullptr;
    }

    bool MaterialEditor::FindSocketAtPosition(const XMFLOAT2& position, MaterialNode*& outNode,
                                              uint32_t& outSocketIndex, bool& outIsInput)
    {
        constexpr float kSocketHitRadius = 10.0f;

        for (auto& node : m_materialGraph.nodes)
        {
            // Check input sockets (left side of node)
            for (uint32_t i = 0; i < node->inputSockets.size(); ++i)
            {
                float socketX = node->position.x;
                float socketY = node->position.y + 30.0f + static_cast<float>(i) * 24.0f;
                float dx = position.x - socketX;
                float dy = position.y - socketY;
                if (dx * dx + dy * dy <= kSocketHitRadius * kSocketHitRadius)
                {
                    outNode = node.get();
                    outSocketIndex = i;
                    outIsInput = true;
                    return true;
                }
            }

            // Check output sockets (right side of node)
            for (uint32_t i = 0; i < node->outputSockets.size(); ++i)
            {
                float socketX = node->position.x + node->size.x;
                float socketY = node->position.y + 30.0f + static_cast<float>(i) * 24.0f;
                float dx = position.x - socketX;
                float dy = position.y - socketY;
                if (dx * dx + dy * dy <= kSocketHitRadius * kSocketHitRadius)
                {
                    outNode = node.get();
                    outSocketIndex = i;
                    outIsInput = false;
                    return true;
                }
            }
        }

        return false;
    }

    bool MaterialEditor::SetupPreviewRendering()
    {
        // Preview rendering requires a D3D11 device — set up render targets
        // for the material preview sphere/cube.
        return m_device != nullptr;
    }

    void MaterialEditor::RenderPreviewToTexture()
    {
        // Render the compiled material onto the preview shape using the
        // generated shader code. Requires D3D11 device and context.
    }

} // namespace SparkEditor
