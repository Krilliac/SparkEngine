/**
 * @file MaterialEditor.cpp
 * @brief Node-based material editor implementation
 */

#include "MaterialEditor.h"
#include "Utils/Validate.h"

#include <imgui.h>

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
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
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
        ImGui::BeginChild("NodePalette", ImVec2(m_nodeListWidth, 0), true);
        ImGui::Text("Node Palette");
        ImGui::Separator();

        for (const auto& [category, nodeTypes] : m_nodeCategories)
        {
            if (ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const auto& nodeType : nodeTypes)
                {
                    const auto& info = GetNodeTypeInfo(nodeType);
                    if (ImGui::Button(info.name.c_str(), ImVec2(m_nodeListWidth - 20.0f, 0)))
                    {
                        // Add node at center of current view
                        XMFLOAT2 center = ScreenToGraph({m_graphViewOffset.x + 400.0f, m_graphViewOffset.y + 300.0f});
                        AddNode(nodeType, center);
                    }
                    if (ImGui::IsItemHovered() && !info.description.empty())
                    {
                        ImGui::SetTooltip("%s", info.description.c_str());
                    }
                }
            }
        }

        ImGui::EndChild();
    }

    void MaterialEditor::RenderMaterialProperties()
    {
        ImGui::BeginChild("MaterialProperties", ImVec2(m_propertiesWidth, 0), true);
        ImGui::Text("Properties");
        ImGui::Separator();

        if (!m_selectedNodes.empty())
        {
            uint32_t selectedID = m_selectedNodes[0];
            MaterialNode* selectedNode = nullptr;
            for (auto& n : m_materialGraph.nodes)
            {
                if (n->id == selectedID)
                {
                    selectedNode = n.get();
                    break;
                }
            }

            if (selectedNode)
            {
                ImGui::Text("Name: %s", selectedNode->name.c_str());
                ImGui::Text("Type: %s", selectedNode->category.c_str());
                if (!selectedNode->description.empty())
                {
                    ImGui::TextWrapped("Description: %s", selectedNode->description.c_str());
                }
                ImGui::Separator();

                // Show editable default values for unconnected input sockets
                for (uint32_t i = 0; i < selectedNode->inputSockets.size(); ++i)
                {
                    auto& socket = selectedNode->inputSockets[i];
                    if (socket.isConnected)
                    {
                        continue;
                    }

                    ImGui::PushID(static_cast<int>(i));
                    switch (socket.type)
                    {
                    case SocketType::FLOAT:
                        ImGui::DragFloat(socket.name.c_str(), &socket.defaultValue.x, 0.01f);
                        break;
                    case SocketType::COLOR:
                    {
                        float color[4] = {socket.defaultValue.x, socket.defaultValue.y, socket.defaultValue.z,
                                          socket.defaultValue.w};
                        if (ImGui::ColorEdit4(socket.name.c_str(), color))
                        {
                            socket.defaultValue = {color[0], color[1], color[2], color[3]};
                        }
                        break;
                    }
                    case SocketType::VECTOR3:
                    {
                        float vec[3] = {socket.defaultValue.x, socket.defaultValue.y, socket.defaultValue.z};
                        if (ImGui::DragFloat3(socket.name.c_str(), vec, 0.01f))
                        {
                            socket.defaultValue = {vec[0], vec[1], vec[2], socket.defaultValue.w};
                        }
                        break;
                    }
                    case SocketType::VECTOR2:
                    {
                        float vec[2] = {socket.defaultValue.x, socket.defaultValue.y};
                        if (ImGui::DragFloat2(socket.name.c_str(), vec, 0.01f))
                        {
                            socket.defaultValue = {vec[0], vec[1], socket.defaultValue.z, socket.defaultValue.w};
                        }
                        break;
                    }
                    case SocketType::VECTOR4:
                    {
                        float vec[4] = {socket.defaultValue.x, socket.defaultValue.y, socket.defaultValue.z,
                                        socket.defaultValue.w};
                        if (ImGui::DragFloat4(socket.name.c_str(), vec, 0.01f))
                        {
                            socket.defaultValue = {vec[0], vec[1], vec[2], vec[3]};
                        }
                        break;
                    }
                    default:
                        ImGui::Text("%s: (no editor)", socket.name.c_str());
                        break;
                    }
                    ImGui::PopID();
                }
            }
        }
        else
        {
            ImGui::TextDisabled("No node selected");
        }

        ImGui::EndChild();
    }

    void MaterialEditor::RenderMaterialPreview()
    {
        ImGui::BeginChild("MaterialPreview", ImVec2(0, m_previewHeight), true);
        ImGui::Text("Material Preview");
        ImGui::Separator();

        if (m_materialGraph.isCompiled)
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Compiled");

#ifdef _WIN32
            RenderPreviewToTexture();

            if (m_previewSRV)
            {
                ImGui::Image(reinterpret_cast<ImTextureID>(m_previewSRV.Get()),
                             ImVec2(m_previewHeight - 60.0f, m_previewHeight - 60.0f));
            }
            else
#endif
            {
                ImGui::TextDisabled("Preview texture not available");
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Not compiled");
        }

        ImGui::Separator();
        ImGui::Text("Shape:");
        ImGui::SameLine();
        if (ImGui::Button("Sphere"))
        {
            SetPreviewShape(MaterialPreview::SPHERE);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cube"))
        {
            SetPreviewShape(MaterialPreview::CUBE);
        }
        ImGui::SameLine();
        if (ImGui::Button("Plane"))
        {
            SetPreviewShape(MaterialPreview::PLANE);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cylinder"))
        {
            SetPreviewShape(MaterialPreview::CYLINDER);
        }

        ImGui::EndChild();
    }

    void MaterialEditor::RenderCompilationOutput()
    {
        ImGui::BeginChild("CompilationOutput", ImVec2(0, 0), true);
        ImGui::Text("Compilation Output");
        ImGui::Separator();

        if (!m_materialGraph.compilationErrors.empty())
        {
            for (const auto& error : m_materialGraph.compilationErrors)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "%s", error.c_str());
            }
        }

        if (m_materialGraph.isCompiled)
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Compilation successful");
            ImGui::Separator();

            if (ImGui::CollapsingHeader("Generated Pixel Shader"))
            {
                ImGui::InputTextMultiline("##PixelShader", &m_materialGraph.pixelShaderCode[0],
                                          m_materialGraph.pixelShaderCode.size() + 1, ImVec2(-1.0f, 200.0f),
                                          ImGuiInputTextFlags_ReadOnly);
            }

            if (ImGui::CollapsingHeader("Generated Vertex Shader"))
            {
                ImGui::InputTextMultiline("##VertexShader", &m_materialGraph.vertexShaderCode[0],
                                          m_materialGraph.vertexShaderCode.size() + 1, ImVec2(-1.0f, 200.0f),
                                          ImGuiInputTextFlags_ReadOnly);
            }
        }

        ImGui::EndChild();
    }

    void MaterialEditor::RenderNode(MaterialNode* node)
    {
        if (!node)
        {
            return;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        XMFLOAT2 nodeScreenPos = GraphToScreen(node->position);
        XMFLOAT2 nodeScreenEnd = GraphToScreen({node->position.x + node->size.x, node->position.y + node->size.y});

        float titleHeight = 24.0f * m_graphViewScale;

        // Draw node body background
        drawList->AddRectFilled(
            ImVec2(nodeScreenPos.x, nodeScreenPos.y), ImVec2(nodeScreenEnd.x, nodeScreenEnd.y),
            IM_COL32(static_cast<int>(node->backgroundColor.x * 255), static_cast<int>(node->backgroundColor.y * 255),
                     static_cast<int>(node->backgroundColor.z * 255), static_cast<int>(node->backgroundColor.w * 255)),
            4.0f);

        // Draw title bar
        drawList->AddRectFilled(
            ImVec2(nodeScreenPos.x, nodeScreenPos.y), ImVec2(nodeScreenEnd.x, nodeScreenPos.y + titleHeight),
            IM_COL32(static_cast<int>(node->titleColor.x * 255), static_cast<int>(node->titleColor.y * 255),
                     static_cast<int>(node->titleColor.z * 255), static_cast<int>(node->titleColor.w * 255)),
            4.0f, ImDrawFlags_RoundCornersTop);

        // Draw selection outline
        if (node->isSelected)
        {
            drawList->AddRect(ImVec2(nodeScreenPos.x, nodeScreenPos.y), ImVec2(nodeScreenEnd.x, nodeScreenEnd.y),
                              IM_COL32(255, 200, 50, 255), 4.0f, 0, 2.0f);
        }

        // Draw node name
        drawList->AddText(ImVec2(nodeScreenPos.x + 6.0f, nodeScreenPos.y + 4.0f), IM_COL32(255, 255, 255, 255),
                          node->name.c_str());

        RenderNodeSockets(node);
    }

    void MaterialEditor::RenderNodeSockets(MaterialNode* node)
    {
        if (!node)
        {
            return;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Draw input sockets (left edge)
        for (uint32_t i = 0; i < node->inputSockets.size(); ++i)
        {
            const auto& socket = node->inputSockets[i];
            float socketX = node->position.x;
            float socketY = node->position.y + 30.0f + static_cast<float>(i) * 24.0f;
            XMFLOAT2 screenPos = GraphToScreen({socketX, socketY});
            float radius = socket.radius * m_graphViewScale;

            ImU32 socketColor = socket.isConnected ? IM_COL32(100, 200, 100, 255) : IM_COL32(150, 150, 150, 255);
            drawList->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), radius, socketColor);

            // Draw socket name to the right of the circle
            drawList->AddText(ImVec2(screenPos.x + radius + 4.0f, screenPos.y - 6.0f), IM_COL32(200, 200, 200, 255),
                              socket.name.c_str());
        }

        // Draw output sockets (right edge)
        for (uint32_t i = 0; i < node->outputSockets.size(); ++i)
        {
            const auto& socket = node->outputSockets[i];
            float socketX = node->position.x + node->size.x;
            float socketY = node->position.y + 30.0f + static_cast<float>(i) * 24.0f;
            XMFLOAT2 screenPos = GraphToScreen({socketX, socketY});
            float radius = socket.radius * m_graphViewScale;

            ImU32 socketColor = socket.isConnected ? IM_COL32(100, 200, 100, 255) : IM_COL32(150, 150, 150, 255);
            drawList->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), radius, socketColor);

            // Draw socket name to the left of the circle
            ImVec2 textSize = ImGui::CalcTextSize(socket.name.c_str());
            drawList->AddText(ImVec2(screenPos.x - radius - 4.0f - textSize.x, screenPos.y - 6.0f),
                              IM_COL32(200, 200, 200, 255), socket.name.c_str());
        }
    }

    void MaterialEditor::RenderConnections()
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (const auto& conn : m_materialGraph.connections)
        {
            // Find source and destination nodes
            MaterialNode* fromNode = nullptr;
            MaterialNode* toNode = nullptr;
            for (auto& n : m_materialGraph.nodes)
            {
                if (n->id == conn.fromNodeID)
                {
                    fromNode = n.get();
                }
                if (n->id == conn.toNodeID)
                {
                    toNode = n.get();
                }
            }

            if (!fromNode || !toNode)
            {
                continue;
            }

            // Calculate output socket position (right side of from-node)
            float fromX = fromNode->position.x + fromNode->size.x;
            float fromY = fromNode->position.y + 30.0f + static_cast<float>(conn.fromSocketIndex) * 24.0f;
            XMFLOAT2 fromScreen = GraphToScreen({fromX, fromY});

            // Calculate input socket position (left side of to-node)
            float toX = toNode->position.x;
            float toY = toNode->position.y + 30.0f + static_cast<float>(conn.toSocketIndex) * 24.0f;
            XMFLOAT2 toScreen = GraphToScreen({toX, toY});

            // Draw bezier curve
            float tangentOffset = std::abs(toScreen.x - fromScreen.x) * 0.5f;
            if (tangentOffset < 50.0f)
            {
                tangentOffset = 50.0f;
            }

            ImU32 lineColor =
                conn.isSelected ? IM_COL32(255, 200, 50, 255)
                                : IM_COL32(static_cast<int>(conn.color.x * 255), static_cast<int>(conn.color.y * 255),
                                           static_cast<int>(conn.color.z * 255), static_cast<int>(conn.color.w * 255));

            drawList->AddBezierCubic(ImVec2(fromScreen.x, fromScreen.y),
                                     ImVec2(fromScreen.x + tangentOffset, fromScreen.y),
                                     ImVec2(toScreen.x - tangentOffset, toScreen.y), ImVec2(toScreen.x, toScreen.y),
                                     lineColor, conn.thickness);
        }
    }

    void MaterialEditor::HandleNodeDragging()
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        XMFLOAT2 graphMousePos = ScreenToGraph({mousePos.x, mousePos.y});

        if (m_isDraggingNode)
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                // Update dragged node position
                for (auto& n : m_materialGraph.nodes)
                {
                    if (n->id == m_draggedNodeID)
                    {
                        n->position.x = graphMousePos.x - m_dragOffset.x;
                        n->position.y = graphMousePos.y - m_dragOffset.y;
                        break;
                    }
                }
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                m_isDraggingNode = false;
                m_draggedNodeID = 0;
            }
        }
        else
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                MaterialNode* hitNode = FindNodeAtPosition(graphMousePos);
                if (hitNode)
                {
                    // Check that we did not click on a socket first
                    MaterialNode* socketNode = nullptr;
                    uint32_t socketIdx = 0;
                    bool isInput = false;
                    if (!FindSocketAtPosition(graphMousePos, socketNode, socketIdx, isInput))
                    {
                        m_isDraggingNode = true;
                        m_draggedNodeID = hitNode->id;
                        m_dragOffset.x = graphMousePos.x - hitNode->position.x;
                        m_dragOffset.y = graphMousePos.y - hitNode->position.y;
                    }
                }
            }
        }
    }

    void MaterialEditor::HandleConnectionCreation()
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        XMFLOAT2 graphMousePos = ScreenToGraph({mousePos.x, mousePos.y});

        if (m_isCreatingConnection)
        {
            // Draw temporary connection line from start socket to mouse
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            MaterialNode* startNode = nullptr;
            for (auto& n : m_materialGraph.nodes)
            {
                if (n->id == m_connectionStartNodeID)
                {
                    startNode = n.get();
                    break;
                }
            }

            if (startNode)
            {
                float startX = 0.0f;
                float startY = 0.0f;
                if (m_connectionStartIsInput)
                {
                    startX = startNode->position.x;
                    startY = startNode->position.y + 30.0f + static_cast<float>(m_connectionStartSocket) * 24.0f;
                }
                else
                {
                    startX = startNode->position.x + startNode->size.x;
                    startY = startNode->position.y + 30.0f + static_cast<float>(m_connectionStartSocket) * 24.0f;
                }
                XMFLOAT2 startScreen = GraphToScreen({startX, startY});

                float tangentOffset = std::abs(mousePos.x - startScreen.x) * 0.5f;
                if (tangentOffset < 50.0f)
                {
                    tangentOffset = 50.0f;
                }

                drawList->AddBezierCubic(ImVec2(startScreen.x, startScreen.y),
                                         ImVec2(startScreen.x + tangentOffset, startScreen.y),
                                         ImVec2(mousePos.x - tangentOffset, mousePos.y), ImVec2(mousePos.x, mousePos.y),
                                         IM_COL32(255, 255, 100, 200), 2.0f);
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                // Check if released on a valid socket
                MaterialNode* endNode = nullptr;
                uint32_t endSocketIndex = 0;
                bool endIsInput = false;

                if (FindSocketAtPosition(graphMousePos, endNode, endSocketIndex, endIsInput))
                {
                    // Connect only if start and end socket directions differ
                    if (m_connectionStartIsInput != endIsInput)
                    {
                        if (m_connectionStartIsInput)
                        {
                            ConnectSockets(endNode->id, endSocketIndex, m_connectionStartNodeID,
                                           m_connectionStartSocket);
                        }
                        else
                        {
                            ConnectSockets(m_connectionStartNodeID, m_connectionStartSocket, endNode->id,
                                           endSocketIndex);
                        }
                    }
                }

                m_isCreatingConnection = false;
                m_connectionStartNodeID = 0;
            }
        }
        else
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                MaterialNode* socketNode = nullptr;
                uint32_t socketIndex = 0;
                bool isInput = false;

                if (FindSocketAtPosition(graphMousePos, socketNode, socketIndex, isInput))
                {
                    m_isCreatingConnection = true;
                    m_connectionStartNodeID = socketNode->id;
                    m_connectionStartSocket = socketIndex;
                    m_connectionStartIsInput = isInput;
                }
            }
        }
    }

    void MaterialEditor::HandleNodeSelection()
    {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            XMFLOAT2 graphMousePos = ScreenToGraph({mousePos.x, mousePos.y});

            MaterialNode* hitNode = FindNodeAtPosition(graphMousePos);

            if (hitNode)
            {
                bool ctrlHeld = ImGui::GetIO().KeyCtrl;

                if (ctrlHeld)
                {
                    // Toggle selection on the clicked node
                    auto it = std::find(m_selectedNodes.begin(), m_selectedNodes.end(), hitNode->id);
                    if (it != m_selectedNodes.end())
                    {
                        m_selectedNodes.erase(it);
                        hitNode->isSelected = false;
                    }
                    else
                    {
                        m_selectedNodes.push_back(hitNode->id);
                        hitNode->isSelected = true;
                    }
                }
                else
                {
                    // Deselect all, then select clicked node
                    for (auto& n : m_materialGraph.nodes)
                    {
                        n->isSelected = false;
                    }
                    m_selectedNodes.clear();
                    m_selectedNodes.push_back(hitNode->id);
                    hitNode->isSelected = true;
                }
            }
            else
            {
                // Clicked on background — deselect all
                for (auto& n : m_materialGraph.nodes)
                {
                    n->isSelected = false;
                }
                m_selectedNodes.clear();
            }
        }
    }

    void MaterialEditor::UpdateNodePreviews()
    {
        // Mark nodes with previews that need regeneration when the graph changes
        if (!m_materialGraph.isCompiled)
        {
            for (auto& node : m_materialGraph.nodes)
            {
                if (node->hasPreview)
                {
                    node->previewTextureID = 0;
                }
            }
        }
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
