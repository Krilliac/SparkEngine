/**
 * @file VisualScriptPanel.cpp
 * @brief Implementation of the node-based visual scripting editor
 */

#include "VisualScriptPanel.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Engine/Scripting/VisualScriptGraphIO.h"
#include "Utils/LogMacros.h"
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace SparkEditor
{

    using namespace Spark::Scripting;

    /// Variable type combo order (VariableUI::typeIndex) to pin kind.
    static constexpr PinKind kVariableKinds[] = {PinKind::Bool,   PinKind::Int,     PinKind::Float,
                                                 PinKind::String, PinKind::Vector3, PinKind::Entity};
    static constexpr const char* kVariableKindNames[] = {"Bool", "Int", "Float", "String", "Vector3", "Entity"};
    static_assert(std::size(kVariableKinds) == std::size(kVariableKindNames));

    struct PaletteCategory
    {
        std::string name;
        std::vector<const ScriptNodePaletteEntry*> entries;
    };

    static std::vector<PaletteCategory> BuildPaletteCategories()
    {
        std::vector<PaletteCategory> categories;
        std::unordered_map<std::string, size_t> categoryToIndex;
        for (const auto& entry : VisualScriptCompiler::GetNodePalette())
        {
            const std::string key = entry.category ? entry.category : "Misc";
            const auto it = categoryToIndex.find(key);
            if (it == categoryToIndex.end())
            {
                categoryToIndex[key] = categories.size();
                categories.push_back(PaletteCategory{key, {}});
            }
            categories[categoryToIndex[key]].entries.push_back(&entry);
        }
        return categories;
    }

    // Node colors by category
    static ImU32 GetNodeColor(ScriptNodeType type)
    {
        auto val = static_cast<uint32_t>(type);
        if (val <= 7)
            return IM_COL32(180, 40, 40, 255); // Events: red
        if (val >= 50 && val <= 53)
            return IM_COL32(60, 60, 160, 255); // Flow: blue
        if (val >= 100 && val <= 108)
            return IM_COL32(40, 140, 80, 255); // Getters: green
        if (val >= 150 && val <= 159)
            return IM_COL32(160, 100, 40, 255); // Actions: orange
        if (val >= 200 && val <= 216)
            return IM_COL32(80, 80, 140, 255); // Math and text: purple
        if (val >= 250 && val <= 259)
            return IM_COL32(100, 140, 100, 255); // Logic: teal
        if (val >= 300 && val <= 301)
            return IM_COL32(140, 140, 40, 255); // Variables: yellow
        if (val >= 350 && val <= 354)
            return IM_COL32(100, 100, 100, 255); // Constants: gray
        if (val == 500)
            return IM_COL32(60, 120, 60, 255); // Comment: dark green
        return IM_COL32(80, 80, 80, 255);
    }

    static const char* GetNodeTitle(ScriptNodeType type)
    {
        return VisualScriptCompiler::GetNodeDisplayName(type);
    }

    // Pin color by kind
    static ImU32 GetPinColor(PinKind kind)
    {
        switch (kind)
        {
        case PinKind::Execution:
            return IM_COL32(255, 255, 255, 255);
        case PinKind::Bool:
            return IM_COL32(180, 40, 40, 255);
        case PinKind::Int:
            return IM_COL32(40, 180, 120, 255);
        case PinKind::Float:
            return IM_COL32(80, 180, 80, 255);
        case PinKind::String:
            return IM_COL32(180, 80, 180, 255);
        case PinKind::Vector3:
            return IM_COL32(220, 180, 40, 255);
        case PinKind::Entity:
            return IM_COL32(40, 120, 220, 255);
        default:
            return IM_COL32(150, 150, 150, 255);
        }
    }

    // Pin label helper — returns a short label for a pin based on its kind and index
    static const char* GetPinLabel(PinKind kind, int index, bool isOutput, ScriptNodeType nodeType)
    {
        // Event outputs
        if (isOutput && index == 0 && kind == PinKind::Execution)
            return "";
        if (isOutput && kind == PinKind::Execution)
        {
            if (nodeType == ScriptNodeType::Branch)
                return index == 0 ? "True" : "False";
            if (nodeType == ScriptNodeType::ForLoop)
                return index == 0 ? "Body" : "Done";
            if (nodeType == ScriptNodeType::Sequence)
            {
                static const char* seqLabels[] = {"0", "1", "2", "3"};
                return (index < 4) ? seqLabels[index] : "";
            }
            return "";
        }
        // Input exec
        if (!isOutput && kind == PinKind::Execution)
            return "";
        // Data pins by kind
        switch (kind)
        {
        case PinKind::Bool:
            return "Bool";
        case PinKind::Int:
            return "Int";
        case PinKind::Float:
            return "Float";
        case PinKind::String:
            return "Str";
        case PinKind::Vector3:
            return "Vec3";
        case PinKind::Entity:
            return "Entity";
        default:
            return "";
        }
    }

    VisualScriptPanel::VisualScriptPanel() : EditorPanel("Visual Script", "VisualScript") {}

    bool VisualScriptPanel::Initialize()
    {
        return true;
    }

    void VisualScriptPanel::Update(float /*deltaTime*/) {}

    void VisualScriptPanel::Render()
    {
        // Top bar: script name and compile button
        RenderCompileBar();

        ImGui::Separator();

        // Three-column layout
        ImGui::Columns(3, "VSColumns", true);
        ImGui::SetColumnWidth(0, 180.0f);
        ImGui::SetColumnWidth(2, 200.0f);

        // Left: Node palette
        RenderNodePalette();

        ImGui::NextColumn();

        // Center: Canvas
        RenderCanvas();

        ImGui::NextColumn();

        // Right: Variables + properties
        RenderVariablesPanel();
        ImGui::Separator();
        RenderNodeProperties();

        ImGui::Columns(1);
    }

    void VisualScriptPanel::Shutdown() {}

    // ========================================================================
    // Node Palette
    // ========================================================================

    void VisualScriptPanel::RenderNodePalette()
    {
        ImGui::Text("Node Palette");

        // Search filter
        static char searchBuf[64] = "";
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##search", "Search nodes...", searchBuf, sizeof(searchBuf));
        ImGui::Separator();

        bool hasSearch = searchBuf[0] != '\0';
        std::string searchLower;
        if (hasSearch)
        {
            searchLower = searchBuf;
            std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);
        }

        const auto categories = BuildPaletteCategories();
        for (const auto& category : categories)
        {
            // If searching, skip empty categories
            bool hasMatch = false;
            if (hasSearch)
            {
                for (const auto* entry : category.entries)
                {
                    std::string name = entry->displayName;
                    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                    if (name.find(searchLower) != std::string::npos)
                    {
                        hasMatch = true;
                        break;
                    }
                }
                if (!hasMatch)
                    continue;
            }

            // Auto-open categories when searching
            bool open = hasSearch ? ImGui::TreeNodeEx(category.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)
                                  : ImGui::TreeNode(category.name.c_str());
            if (open)
            {
                for (const auto* entry : category.entries)
                {
                    if (hasSearch)
                    {
                        std::string name = entry->displayName;
                        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                        if (name.find(searchLower) == std::string::npos)
                            continue;
                    }
                    if (ImGui::Selectable(entry->displayName))
                    {
                        // Add node at center of canvas view
                        float cx = -m_canvasOffsetX + 300.0f;
                        float cy = -m_canvasOffsetY + 200.0f;
                        AddNodeAtPosition(entry->type, cx, cy);
                    }
                }
                ImGui::TreePop();
            }
        }
    }

    // ========================================================================
    // Canvas
    // ========================================================================

    void VisualScriptPanel::RenderCanvas()
    {
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        canvasSize.y = std::max(canvasSize.y, 200.0f);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Canvas background
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                IM_COL32(30, 30, 30, 255));

        // Grid
        float gridSize = 32.0f * m_canvasZoom;
        for (float x = std::fmod(m_canvasOffsetX, gridSize); x < canvasSize.x; x += gridSize)
        {
            drawList->AddLine(ImVec2(canvasPos.x + x, canvasPos.y), ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y),
                              IM_COL32(50, 50, 50, 255));
        }
        for (float y = std::fmod(m_canvasOffsetY, gridSize); y < canvasSize.y; y += gridSize)
        {
            drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + y), ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y),
                              IM_COL32(50, 50, 50, 255));
        }

        // Clip to canvas area
        drawList->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), true);

        // Store canvas origin for pin position calculations
        m_canvasOriginX = canvasPos.x;
        m_canvasOriginY = canvasPos.y;

        // Draw connections
        RenderConnections();

        // Draw pending connection line while dragging
        RenderPendingConnection();

        // Draw nodes
        for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
        {
            RenderNode(i);
        }

        drawList->PopClipRect();

        // Handle canvas input (pan, zoom, context menu)
        ImGui::SetCursorScreenPos(canvasPos);
        ImGui::InvisibleButton("canvas", canvasSize);

        if (ImGui::IsItemHovered())
        {
            HandleCanvasInput();
        }

        // Context menu
        if (m_showContextMenu)
        {
            ImGui::OpenPopup("CanvasContextMenu");
            m_showContextMenu = false;
        }
        if (ImGui::BeginPopup("CanvasContextMenu"))
        {
            AddContextMenuNode();
            ImGui::EndPopup();
        }

        // Info text
        ImGui::SetCursorScreenPos(ImVec2(canvasPos.x + 5, canvasPos.y + canvasSize.y - 20));
        ImGui::TextDisabled("Nodes: %zu | Connections: %zu | Right-click to add nodes", m_nodes.size(),
                            m_connections.size());
    }

    void VisualScriptPanel::RenderNode(int nodeIndex)
    {
        auto& nodeUI = m_nodes[nodeIndex];
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        float nx = canvasPos.x + (nodeUI.posX + m_canvasOffsetX) * m_canvasZoom;
        float ny = canvasPos.y + (nodeUI.posY + m_canvasOffsetY) * m_canvasZoom;
        float nw = nodeUI.width * m_canvasZoom;
        float nh = nodeUI.height * m_canvasZoom;

        ImU32 nodeColor = GetNodeColor(nodeUI.node.type);
        ImU32 headerColor = nodeColor;
        ImU32 bodyColor = IM_COL32(40, 40, 40, 230);

        // Node body
        drawList->AddRectFilled(ImVec2(nx, ny), ImVec2(nx + nw, ny + nh), bodyColor, 4.0f);

        // Header
        drawList->AddRectFilled(ImVec2(nx, ny), ImVec2(nx + nw, ny + 24.0f * m_canvasZoom), headerColor, 4.0f);

        // Border
        ImU32 borderColor = nodeUI.selected ? IM_COL32(255, 200, 50, 255) : IM_COL32(80, 80, 80, 255);
        drawList->AddRect(ImVec2(nx, ny), ImVec2(nx + nw, ny + nh), borderColor, 4.0f, 0, 2.0f);

        // Title
        const char* title = GetNodeTitle(nodeUI.node.type);
        drawList->AddText(ImVec2(nx + 8, ny + 4), IM_COL32(255, 255, 255, 255), title);

        // Comment nodes: render text body
        if (nodeUI.node.type == ScriptNodeType::Comment)
        {
            auto it = nodeUI.node.properties.find("text");
            const char* commentText = (it != nodeUI.node.properties.end()) ? it->second.c_str() : "Comment";
            drawList->AddText(ImVec2(nx + 8, ny + 26.0f * m_canvasZoom), IM_COL32(200, 230, 200, 220), commentText);
        }

        // Input pins (with click detection for connection dragging)
        float pinRadius = 5.0f * m_canvasZoom;
        float pinY = ny + 30.0f * m_canvasZoom;
        ImGuiIO& pinIO = ImGui::GetIO();
        for (size_t p = 0; p < nodeUI.node.inputs.size(); ++p)
        {
            ImVec2 pinPos(nx, pinY);
            ImU32 pinColor = GetPinColor(nodeUI.node.inputs[p].kind);
            drawList->AddCircleFilled(pinPos, pinRadius, pinColor);

            // Pin label
            const char* label = GetPinLabel(nodeUI.node.inputs[p].kind, static_cast<int>(p), false, nodeUI.node.type);
            if (label[0] != '\0' && m_canvasZoom > 0.5f)
            {
                drawList->AddText(ImVec2(pinPos.x + pinRadius + 3.0f, pinPos.y - 6.0f * m_canvasZoom),
                                  IM_COL32(180, 180, 180, 200), label);
            }

            // Hover highlight
            float dx = pinIO.MousePos.x - pinPos.x;
            float dy = pinIO.MousePos.y - pinPos.y;
            if (dx * dx + dy * dy < (pinRadius + 4.0f) * (pinRadius + 4.0f))
            {
                drawList->AddCircle(pinPos, pinRadius + 3.0f, IM_COL32(255, 255, 255, 200), 12, 2.0f);
                if (ImGui::IsMouseClicked(0))
                {
                    if (m_isDrawingConnection)
                        TryCompleteConnection(nodeIndex, static_cast<int>(p), false);
                    else
                        TryStartConnection(nodeIndex, static_cast<int>(p), false);
                }
            }
            pinY += 18.0f * m_canvasZoom;
        }

        // Output pins (with click detection for connection dragging)
        pinY = ny + 30.0f * m_canvasZoom;
        for (size_t p = 0; p < nodeUI.node.outputs.size(); ++p)
        {
            ImVec2 pinPos(nx + nw, pinY);
            ImU32 pinColor = GetPinColor(nodeUI.node.outputs[p].kind);
            drawList->AddCircleFilled(pinPos, pinRadius, pinColor);

            // Pin label (right-aligned)
            const char* outLabel =
                GetPinLabel(nodeUI.node.outputs[p].kind, static_cast<int>(p), true, nodeUI.node.type);
            if (outLabel[0] != '\0' && m_canvasZoom > 0.5f)
            {
                float textWidth = ImGui::CalcTextSize(outLabel).x;
                drawList->AddText(ImVec2(pinPos.x - pinRadius - 3.0f - textWidth, pinPos.y - 6.0f * m_canvasZoom),
                                  IM_COL32(180, 180, 180, 200), outLabel);
            }

            float dx = pinIO.MousePos.x - pinPos.x;
            float dy = pinIO.MousePos.y - pinPos.y;
            if (dx * dx + dy * dy < (pinRadius + 4.0f) * (pinRadius + 4.0f))
            {
                drawList->AddCircle(pinPos, pinRadius + 3.0f, IM_COL32(255, 255, 255, 200), 12, 2.0f);
                if (ImGui::IsMouseClicked(0))
                {
                    if (m_isDrawingConnection)
                        TryCompleteConnection(nodeIndex, static_cast<int>(p), true);
                    else
                        TryStartConnection(nodeIndex, static_cast<int>(p), true);
                }
            }
            pinY += 18.0f * m_canvasZoom;
        }

        // Node interaction (selection, dragging)
        ImGui::SetCursorScreenPos(ImVec2(nx, ny));
        ImGui::InvisibleButton(("node_" + std::to_string(nodeIndex)).c_str(), ImVec2(nw, nh));

        if (ImGui::IsItemClicked(0))
        {
            // Deselect others
            for (auto& n : m_nodes)
            {
                n.selected = false;
            }
            nodeUI.selected = true;
            m_selectedNode = nodeIndex;
        }

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0))
        {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            nodeUI.posX += delta.x / m_canvasZoom;
            nodeUI.posY += delta.y / m_canvasZoom;
        }

        // Delete on key press
        if (nodeUI.selected && ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            // Remove connections to/from this node
            uint32_t nodeId = nodeUI.node.id;
            std::erase_if(m_connections, [nodeId](const ConnectionUI& c)
                          { return c.connection.fromNode == nodeId || c.connection.toNode == nodeId; });
            m_nodes.erase(m_nodes.begin() + nodeIndex);
            m_selectedNode = -1;
        }
    }

    void VisualScriptPanel::RenderConnections()
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();

        for (const auto& connUI : m_connections)
        {
            const auto& conn = connUI.connection;

            // Find source and target nodes
            const NodeUI* fromNode = nullptr;
            const NodeUI* toNode = nullptr;
            for (const auto& n : m_nodes)
            {
                if (n.node.id == conn.fromNode)
                    fromNode = &n;
                if (n.node.id == conn.toNode)
                    toNode = &n;
            }
            if (!fromNode || !toNode)
            {
                continue;
            }

            // Calculate pin positions
            float fromX = canvasPos.x + (fromNode->posX + fromNode->width + m_canvasOffsetX) * m_canvasZoom;
            float fromY =
                canvasPos.y + (fromNode->posY + 30.0f + conn.fromPin * 18.0f + m_canvasOffsetY) * m_canvasZoom;
            float toX = canvasPos.x + (toNode->posX + m_canvasOffsetX) * m_canvasZoom;
            float toY = canvasPos.y + (toNode->posY + 30.0f + conn.toPin * 18.0f + m_canvasOffsetY) * m_canvasZoom;

            // Determine wire color from source pin
            ImU32 wireColor = IM_COL32(180, 180, 180, 200);
            if (conn.fromPin < fromNode->node.outputs.size())
            {
                wireColor = GetPinColor(fromNode->node.outputs[conn.fromPin].kind);
            }

            // Draw bezier curve
            float dx = std::abs(toX - fromX) * 0.5f;
            drawList->AddBezierCubic(ImVec2(fromX, fromY), ImVec2(fromX + dx, fromY), ImVec2(toX - dx, toY),
                                     ImVec2(toX, toY), wireColor, 2.0f * m_canvasZoom);
        }
    }

    void VisualScriptPanel::HandleCanvasInput()
    {
        ImGuiIO& io = ImGui::GetIO();

        // Pan with middle mouse button
        if (ImGui::IsMouseDragging(2))
        {
            m_canvasOffsetX += io.MouseDelta.x / m_canvasZoom;
            m_canvasOffsetY += io.MouseDelta.y / m_canvasZoom;
        }

        // Zoom with scroll wheel
        if (std::abs(io.MouseWheel) > 0.0f)
        {
            m_canvasZoom *= (io.MouseWheel > 0) ? 1.1f : 0.9f;
            m_canvasZoom = std::clamp(m_canvasZoom, 0.25f, 3.0f);
        }

        // Right-click context menu
        if (ImGui::IsMouseClicked(1))
        {
            m_showContextMenu = true;
            ImVec2 canvasPos = ImGui::GetCursorScreenPos();
            m_contextMenuX = (io.MousePos.x - canvasPos.x) / m_canvasZoom - m_canvasOffsetX;
            m_contextMenuY = (io.MousePos.y - canvasPos.y) / m_canvasZoom - m_canvasOffsetY;
        }

        // Cancel connection drawing on right-click or Escape
        if (m_isDrawingConnection)
        {
            if (ImGui::IsMouseClicked(1) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_isDrawingConnection = false;
                m_connectionSourceNode = -1;
            }
        }

        // Click on empty space deselects
        if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered())
        {
            if (m_isDrawingConnection)
            {
                m_isDrawingConnection = false;
                m_connectionSourceNode = -1;
            }
            for (auto& n : m_nodes)
            {
                n.selected = false;
            }
            m_selectedNode = -1;
        }
    }

    void VisualScriptPanel::AddContextMenuNode()
    {
        const auto categories = BuildPaletteCategories();
        for (const auto& category : categories)
        {
            if (ImGui::BeginMenu(category.name.c_str()))
            {
                for (const auto* entry : category.entries)
                {
                    if (ImGui::MenuItem(entry->displayName))
                    {
                        AddNodeAtPosition(entry->type, m_contextMenuX, m_contextMenuY);
                    }
                }
                ImGui::EndMenu();
            }
        }
    }

    void VisualScriptPanel::AddNodeAtPosition(ScriptNodeType type, float x, float y)
    {
        if (m_nextNodeId > VisualScriptGraphIO::kMaxNodeId)
        {
            // Ids never wrap: a reused id would make the graph unsavable and drop a node at compile time.
            m_compileErrors = {"Cannot add node: node ids are exhausted (largest id " +
                               std::to_string(VisualScriptGraphIO::kMaxNodeId) + ")"};
            return;
        }

        NodeUI nodeUI;
        nodeUI.node.id = m_nextNodeId++;
        nodeUI.node.type = type;
        nodeUI.posX = x;
        nodeUI.posY = y;

        // Set up default pins based on node type
        auto addInput = [&](PinKind kind, const std::string& defStr = "", float defVal = 0.0f)
        {
            ScriptPin pin;
            pin.kind = kind;
            pin.defaultValue[0] = defVal;
            pin.defaultString = defStr;
            nodeUI.node.inputs.push_back(std::move(pin));
        };
        auto addOutput = [&](PinKind kind)
        {
            ScriptPin pin;
            pin.kind = kind;
            nodeUI.node.outputs.push_back(std::move(pin));
        };

        // --- Pin setup by node type ---
        switch (type)
        {
        // Events: execution output (+ optional data outputs)
        case ScriptNodeType::OnStart:
        case ScriptNodeType::OnCustomEvent:
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::OnUpdate:
            addOutput(PinKind::Execution);
            addOutput(PinKind::Float); // DeltaTime
            break;
        case ScriptNodeType::OnTriggerEnter:
        case ScriptNodeType::OnTriggerExit:
        case ScriptNodeType::OnCollision:
            addOutput(PinKind::Execution);
            addOutput(PinKind::Entity); // Other entity
            break;
        case ScriptNodeType::OnDamaged:
            addOutput(PinKind::Execution);
            addOutput(PinKind::Float); // Damage amount
            break;
        case ScriptNodeType::OnKeyPress:
            addOutput(PinKind::Execution);
            break;

        // Flow control
        case ScriptNodeType::Branch:
            addInput(PinKind::Execution);
            addInput(PinKind::Bool);       // Condition
            addOutput(PinKind::Execution); // True
            addOutput(PinKind::Execution); // False
            break;
        case ScriptNodeType::ForLoop:
            addInput(PinKind::Execution);
            addInput(PinKind::Int, "", 0.0f);  // Start
            addInput(PinKind::Int, "", 10.0f); // End
            addOutput(PinKind::Execution);     // Loop Body
            addOutput(PinKind::Int);           // Index
            addOutput(PinKind::Execution);     // Completed
            break;
        case ScriptNodeType::Sequence:
            addInput(PinKind::Execution);
            addOutput(PinKind::Execution); // Then 0
            addOutput(PinKind::Execution); // Then 1
            addOutput(PinKind::Execution); // Then 2
            break;
        case ScriptNodeType::DoNothing:
            addInput(PinKind::Execution);
            addOutput(PinKind::Execution);
            break;

        // Getters
        case ScriptNodeType::GetPosition:
            addInput(PinKind::Entity);
            addOutput(PinKind::Vector3);
            break;
        case ScriptNodeType::GetRotation:
            addInput(PinKind::Entity);
            addOutput(PinKind::Vector3);
            break;
        case ScriptNodeType::GetHealth:
            addInput(PinKind::Entity);
            addOutput(PinKind::Float);
            break;
        case ScriptNodeType::GetSpeed:
            addInput(PinKind::Entity);
            addOutput(PinKind::Float);
            break;
        case ScriptNodeType::GetEntityByName:
            addOutput(PinKind::Entity);
            break;
        case ScriptNodeType::GetSelf:
            addOutput(PinKind::Entity);
            break;
        case ScriptNodeType::GetKeyDown:
        case ScriptNodeType::GetKey:
            addOutput(PinKind::Bool);
            break;
        case ScriptNodeType::GetDeltaTime:
            addOutput(PinKind::Float);
            break;

        // Actions (all have Exec in → Exec out)
        case ScriptNodeType::SetPosition:
            addInput(PinKind::Execution);
            addInput(PinKind::Entity);
            addInput(PinKind::Vector3);
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::SetRotation:
            addInput(PinKind::Execution);
            addInput(PinKind::Entity);
            addInput(PinKind::Vector3);
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::SetHealth:
            addInput(PinKind::Execution);
            addInput(PinKind::Entity);
            addInput(PinKind::Float);
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::ApplyForce:
            addInput(PinKind::Execution);
            addInput(PinKind::Entity);
            addInput(PinKind::Vector3);
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::PlaySound:
            addInput(PinKind::Execution);
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::PlayAnimation:
            addInput(PinKind::Execution);
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::SpawnEntity:
            addInput(PinKind::Execution);
            addOutput(PinKind::Execution);
            addOutput(PinKind::Entity);
            break;
        case ScriptNodeType::DestroyEntity:
            addInput(PinKind::Execution);
            addInput(PinKind::Entity);
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::PrintMessage:
            addInput(PinKind::Execution);
            addInput(PinKind::String, "Hello!");
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::FireEvent:
            addInput(PinKind::Execution);
            addOutput(PinKind::Execution);
            break;

        // Math (pure — no execution pins)
        case ScriptNodeType::Add:
        case ScriptNodeType::Subtract:
        case ScriptNodeType::Multiply:
        case ScriptNodeType::Divide:
            addInput(PinKind::Float, "", 0.0f);
            addInput(PinKind::Float, "", 0.0f);
            addOutput(PinKind::Float);
            break;
        case ScriptNodeType::Negate:
        case ScriptNodeType::Abs:
            addInput(PinKind::Float, "", 0.0f);
            addOutput(PinKind::Float);
            break;
        case ScriptNodeType::Lerp:
        case ScriptNodeType::Clamp:
            addInput(PinKind::Float, "", 0.0f);
            addInput(PinKind::Float, "", 0.0f);
            addInput(PinKind::Float, "", 0.5f);
            addOutput(PinKind::Float);
            break;
        case ScriptNodeType::Random:
            addOutput(PinKind::Float);
            break;
        case ScriptNodeType::RandomRange:
            addInput(PinKind::Float, "", 0.0f);
            addInput(PinKind::Float, "", 1.0f);
            addOutput(PinKind::Float);
            break;
        case ScriptNodeType::Normalize:
            addInput(PinKind::Vector3);
            addOutput(PinKind::Vector3);
            break;
        case ScriptNodeType::DotProduct:
        case ScriptNodeType::Distance:
            addInput(PinKind::Vector3);
            addInput(PinKind::Vector3);
            addOutput(PinKind::Float);
            break;

        case ScriptNodeType::BreakVector3:
            addInput(PinKind::Vector3);
            addOutput(PinKind::Float); // X
            addOutput(PinKind::Float); // Y
            addOutput(PinKind::Float); // Z
            break;
        case ScriptNodeType::MakeVector3:
            addInput(PinKind::Float); // X
            addInput(PinKind::Float); // Y
            addInput(PinKind::Float); // Z
            addOutput(PinKind::Vector3);
            break;
        case ScriptNodeType::ToInt:
            addInput(PinKind::Float);
            addOutput(PinKind::Int);
            break;
        case ScriptNodeType::AppendString:
            addInput(PinKind::String);
            addInput(PinKind::Any); // Value appended as text
            addOutput(PinKind::String);
            break;

        // Logic
        case ScriptNodeType::Select:
            addInput(PinKind::Bool);  // Condition
            addInput(PinKind::Float); // A (condition true)
            addInput(PinKind::Float); // B (condition false)
            addOutput(PinKind::Float);
            break;
        case ScriptNodeType::And:
        case ScriptNodeType::Or:
            addInput(PinKind::Bool);
            addInput(PinKind::Bool);
            addOutput(PinKind::Bool);
            break;
        case ScriptNodeType::Not:
            addInput(PinKind::Bool);
            addOutput(PinKind::Bool);
            break;
        case ScriptNodeType::Equal:
        case ScriptNodeType::NotEqual:
        case ScriptNodeType::Greater:
        case ScriptNodeType::Less:
        case ScriptNodeType::GreaterEqual:
        case ScriptNodeType::LessEqual:
            addInput(PinKind::Float);
            addInput(PinKind::Float);
            addOutput(PinKind::Bool);
            break;

        // Variables
        case ScriptNodeType::GetVariable:
            addOutput(PinKind::Float); // Type resolved at compile time
            break;
        case ScriptNodeType::SetVariable:
            addInput(PinKind::Execution);
            addInput(PinKind::Float); // Value
            addOutput(PinKind::Execution);
            break;

        // Custom events & functions
        case ScriptNodeType::DefineCustomEvent:
            addOutput(PinKind::Execution);
            break;
        case ScriptNodeType::CallFunction:
            addInput(PinKind::Execution);
            addInput(PinKind::Float); // Argument (user can add more)
            addOutput(PinKind::Execution);
            addOutput(PinKind::Float); // Return value
            break;
        case ScriptNodeType::ReturnValue:
            addInput(PinKind::Execution);
            addInput(PinKind::Float); // Value to return
            break;

        // Comment (no pins, just a visual box)
        case ScriptNodeType::Comment:
            nodeUI.width = 200.0f;
            nodeUI.height = 60.0f;
            break;

        // Constants
        case ScriptNodeType::ConstFloat:
            addOutput(PinKind::Float);
            break;
        case ScriptNodeType::ConstInt:
            addOutput(PinKind::Int);
            break;
        case ScriptNodeType::ConstBool:
            addOutput(PinKind::Bool);
            break;
        case ScriptNodeType::ConstString:
            addOutput(PinKind::String);
            break;
        case ScriptNodeType::ConstVector3:
            addOutput(PinKind::Vector3);
            break;

        default:
            break;
        }

        // Calculate height based on pin count
        int maxPins =
            std::max(static_cast<int>(nodeUI.node.inputs.size()), static_cast<int>(nodeUI.node.outputs.size()));
        nodeUI.height = 30.0f + maxPins * 18.0f + 10.0f;
        nodeUI.height = std::max(nodeUI.height, 50.0f);

        m_nodes.push_back(std::move(nodeUI));
    }

    // ========================================================================
    // Variables Panel
    // ========================================================================

    void VisualScriptPanel::RenderVariablesPanel()
    {
        ImGui::Text("Variables");
        ImGui::Separator();

        for (int i = 0; i < static_cast<int>(m_variables.size()); ++i)
        {
            ImGui::PushID(i);
            auto& var = m_variables[i];

            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputText("##name", var.name, sizeof(var.name));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.0f);
            ImGui::Combo("##type", &var.typeIndex, kVariableKindNames, static_cast<int>(std::size(kVariableKindNames)));
            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
            {
                m_variables.erase(m_variables.begin() + i);
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }

        if (ImGui::SmallButton("+ Variable"))
        {
            // First free varN: a count-based name would repeat after a delete, and the loader rejects duplicates.
            VariableUI var{};
            for (size_t suffix = m_variables.size();; ++suffix)
            {
                std::snprintf(var.name, sizeof(var.name), "var%zu", suffix);
                const bool taken =
                    std::any_of(m_variables.begin(), m_variables.end(), [&var](const VariableUI& existing)
                                { return std::strcmp(existing.name, var.name) == 0; });
                if (!taken)
                    break;
            }
            m_variables.push_back(var);
        }
    }

    // ========================================================================
    // Node Properties
    // ========================================================================

    void VisualScriptPanel::RenderNodeProperties()
    {
        ImGui::Text("Properties");
        ImGui::Separator();

        if (m_selectedNode < 0 || m_selectedNode >= static_cast<int>(m_nodes.size()))
        {
            ImGui::TextDisabled("Select a node");
            return;
        }

        auto& nodeUI = m_nodes[m_selectedNode];
        ImGui::Text("Type: %s", GetNodeTitle(nodeUI.node.type));
        ImGui::Text("ID: %u", nodeUI.node.id);

        // Edit default values for input pins
        for (size_t i = 0; i < nodeUI.node.inputs.size(); ++i)
        {
            auto& pin = nodeUI.node.inputs[i];
            ImGui::PushID(static_cast<int>(i));

            switch (pin.kind)
            {
            case PinKind::Float:
                ImGui::DragFloat("##val", &pin.defaultValue[0], 0.1f);
                break;
            case PinKind::Int:
            {
                // The .vscript format stores Int defaults in a float, exact only within +-2^24.
                constexpr int kIntDefaultLimit = 1 << 24;
                int val = static_cast<int>(pin.defaultValue[0]);
                if (ImGui::DragInt("##val", &val, 1.0f, -kIntDefaultLimit, kIntDefaultLimit, "%d",
                                   ImGuiSliderFlags_AlwaysClamp))
                {
                    pin.defaultValue[0] = static_cast<float>(val);
                }
                break;
            }
            case PinKind::Bool:
            {
                bool val = pin.defaultValue[0] != 0.0f;
                if (ImGui::Checkbox("##val", &val))
                {
                    pin.defaultValue[0] = val ? 1.0f : 0.0f;
                }
                break;
            }
            case PinKind::String:
            {
                char buf[128];
                std::strncpy(buf, pin.defaultString.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                if (ImGui::InputText("##val", buf, sizeof(buf)))
                {
                    pin.defaultString = buf;
                }
                break;
            }
            default:
                break;
            }

            ImGui::PopID();
        }

        // Node-specific properties
        auto& props = nodeUI.node.properties;

        // Key name dropdown for input nodes
        if (nodeUI.node.type == ScriptNodeType::OnKeyPress || nodeUI.node.type == ScriptNodeType::GetKeyDown ||
            nodeUI.node.type == ScriptNodeType::GetKey)
        {
            static const char* keyNames[] = {"W", "A", "S",   "D",      "Space", "LeftShift", "E",         "F",
                                             "R", "Q", "Tab", "Escape", "Enter", "LeftCtrl",  "LeftMouse", "RightMouse",
                                             "1", "2", "3",   "4",      "Up",    "Down",      "Left",      "Right"};
            static constexpr int keyCount = static_cast<int>(std::size(keyNames));

            auto it = props.find("key");
            std::string currentKey = (it != props.end()) ? it->second : "Space";
            int selectedKey = 4; // Default: Space
            for (int k = 0; k < keyCount; k++)
            {
                if (currentKey == keyNames[k])
                {
                    selectedKey = k;
                    break;
                }
            }
            if (ImGui::Combo("Key", &selectedKey, keyNames, keyCount))
                props["key"] = keyNames[selectedKey];
        }

        // Sound name for PlaySound
        if (nodeUI.node.type == ScriptNodeType::PlaySound)
        {
            auto it = props.find("sound");
            char buf[128];
            std::strncpy(buf, (it != props.end()) ? it->second.c_str() : "", sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText("Sound", buf, sizeof(buf)))
                props["sound"] = buf;
        }

        // Animation name for PlayAnimation
        if (nodeUI.node.type == ScriptNodeType::PlayAnimation)
        {
            auto it = props.find("animation");
            char buf[128];
            std::strncpy(buf, (it != props.end()) ? it->second.c_str() : "", sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText("Animation", buf, sizeof(buf)))
                props["animation"] = buf;
        }

        // Event name for FireEvent
        if (nodeUI.node.type == ScriptNodeType::FireEvent)
        {
            auto it = props.find("event");
            char buf[128];
            std::strncpy(buf, (it != props.end()) ? it->second.c_str() : "", sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText("Event", buf, sizeof(buf)))
                props["event"] = buf;
        }

        // Entity name for SpawnEntity and GetEntityByName
        if (nodeUI.node.type == ScriptNodeType::SpawnEntity || nodeUI.node.type == ScriptNodeType::GetEntityByName)
        {
            auto it = props.find("name");
            char buf[128];
            std::strncpy(buf, (it != props.end()) ? it->second.c_str() : "Entity", sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText("Entity Name", buf, sizeof(buf)))
                props["name"] = buf;
        }

        // Function name for CallFunction
        if (nodeUI.node.type == ScriptNodeType::CallFunction)
        {
            auto it = props.find("function");
            char buf[128];
            std::strncpy(buf, (it != props.end()) ? it->second.c_str() : "myFunction", sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText("Function", buf, sizeof(buf)))
                props["function"] = buf;
        }

        // Variable name for Get/Set Variable
        if (nodeUI.node.type == ScriptNodeType::GetVariable || nodeUI.node.type == ScriptNodeType::SetVariable)
        {
            auto it = props.find("name");
            char buf[128];
            std::strncpy(buf, (it != props.end()) ? it->second.c_str() : "var0", sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText("Variable", buf, sizeof(buf)))
                props["name"] = buf;

            // Show dropdown of declared variables
            if (!m_variables.empty())
            {
                if (ImGui::BeginCombo("##varlist", (it != props.end()) ? it->second.c_str() : "select..."))
                {
                    for (const auto& v : m_variables)
                    {
                        if (ImGui::Selectable(v.name))
                            props["name"] = v.name;
                    }
                    ImGui::EndCombo();
                }
            }
        }

        // Comment text
        if (nodeUI.node.type == ScriptNodeType::Comment)
        {
            auto it = props.find("text");
            char buf[256];
            std::strncpy(buf, (it != props.end()) ? it->second.c_str() : "Comment", sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputTextMultiline("##comment", buf, sizeof(buf), ImVec2(-1, 60)))
                props["text"] = buf;
        }
    }

    // ========================================================================
    // Compile Bar
    // ========================================================================

    void VisualScriptPanel::RenderCompileBar()
    {
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Script Name", m_scriptName, sizeof(m_scriptName));
        ImGui::SameLine();

        if (ImGui::Button("Compile"))
        {
            CompileGraph();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            std::string savePath = std::string(m_savePath) + std::string(m_scriptName) + ".vscript";
            SaveGraph(savePath);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load"))
        {
            std::string loadPath = std::string(m_savePath) + std::string(m_scriptName) + ".vscript";
            LoadGraph(loadPath);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Debug", &m_debugCompile);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Insert trace calls for each node (view in Script Debugger panel)");

        ImGui::SameLine();
        if (m_compileSuccess)
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Compiled OK");
        }
        else if (!m_compileErrors.empty())
        {
            ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "Errors: %zu", m_compileErrors.size());
        }

        // Show errors if any
        if (!m_compileErrors.empty())
        {
            for (const auto& err : m_compileErrors)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  %s", err.c_str());
            }
        }

        // Generated code preview (collapsible)
        if (!m_lastCompiledSource.empty())
        {
            if (ImGui::CollapsingHeader("Generated AngelScript"))
            {
                ImGui::BeginChild("CodePreview", ImVec2(0, 200), true);
                ImGui::TextUnformatted(m_lastCompiledSource.c_str());
                ImGui::EndChild();
            }
        }
    }

    // ========================================================================
    // Compilation
    // ========================================================================

    Spark::Scripting::VisualScriptGraph VisualScriptPanel::BuildGraph() const
    {
        VisualScriptGraph graph;
        graph.className = m_scriptName;
        graph.description = m_description;
        for (const auto& nodeUI : m_nodes)
        {
            graph.nodes.push_back(nodeUI.node);
            graph.nodes.back().editorX = nodeUI.posX;
            graph.nodes.back().editorY = nodeUI.posY;
        }
        for (const auto& connUI : m_connections)
        {
            graph.connections.push_back(connUI.connection);
        }
        for (const auto& varUI : m_variables)
        {
            VariableDecl var;
            var.name = varUI.name;
            var.type = kVariableKinds[std::clamp(varUI.typeIndex, 0, static_cast<int>(std::size(kVariableKinds)) - 1)];
            var.defaultValue = varUI.defaultValue;
            graph.variables.push_back(std::move(var));
        }
        graph.functions = m_functions;
        graph.customEvents = m_customEvents;
        return graph;
    }

    void VisualScriptPanel::CompileGraph()
    {
        const VisualScriptGraph graph = BuildGraph();
        auto result = VisualScriptCompiler::Compile(graph, m_debugCompile);
        m_compileErrors = result.errors;
        m_compileSuccess = result.success;
        m_lastCompiledSource = result.angelScriptSource;

        if (result.success)
        {
            // Write the generated .as byte for byte (binary: no CRLF translation), so a
            // graph regenerates exactly the script checked in beside it.
            std::string outPath = std::string(m_savePath) + std::string(m_scriptName) + ".as";
            std::ofstream file(outPath, std::ios::binary);
            if (file.is_open())
            {
                file << result.angelScriptSource;
                file.close();
            }

            // Load into AngelScript engine for execution
            auto* asEngine = AngelScriptEngine::GetInstance();
            if (asEngine)
            {
                std::string moduleName = m_scriptName;
                if (!asEngine->CompileScriptFromString(result.angelScriptSource, moduleName))
                {
                    m_compileErrors.push_back("AngelScript: " + asEngine->GetLastError());
                    m_compileSuccess = false;
                }
            }
        }
    }

    void VisualScriptPanel::SaveGraph(const std::string& path)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "VisualScriptPanel::SaveGraph — '%s' (%zu nodes, %zu connections)",
                       path.c_str(), m_nodes.size(), m_connections.size());
        if (auto saved = VisualScriptGraphIO::SaveFile(path, BuildGraph()); !saved)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "SaveGraph: %s", saved.error().c_str());
            m_compileErrors = {"Save failed: " + saved.error()};
        }
    }

    void VisualScriptPanel::LoadGraph(const std::string& path)
    {
        auto graph = VisualScriptGraphIO::LoadFile(path);
        if (!graph)
        {
            // The editor keeps the current graph; the file is reported, never half-applied.
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "LoadGraph: %s", graph.error().c_str());
            m_compileErrors = {"Load failed: " + graph.error()};
            return;
        }

        m_nodes.clear();
        m_connections.clear();
        m_variables.clear();
        m_selectedNode = -1;
        m_nextNodeId = 1;
        m_compileErrors.clear();

        for (auto& node : graph->nodes)
        {
            NodeUI nodeUI;
            nodeUI.posX = node.editorX;
            nodeUI.posY = node.editorY;
            if (node.type == ScriptNodeType::Comment)
            {
                nodeUI.width = 200.0f;
                nodeUI.height = 60.0f;
            }
            m_nextNodeId = std::max(m_nextNodeId, node.id + 1);
            nodeUI.node = std::move(node);
            m_nodes.push_back(std::move(nodeUI));
        }
        for (const auto& connection : graph->connections)
        {
            m_connections.push_back(ConnectionUI{connection});
        }
        for (const auto& var : graph->variables)
        {
            VariableUI varUI{};
            std::strncpy(varUI.name, var.name.c_str(), sizeof(varUI.name) - 1);
            const auto* kind = std::find(std::begin(kVariableKinds), std::end(kVariableKinds), var.type);
            varUI.typeIndex =
                kind != std::end(kVariableKinds) ? static_cast<int>(kind - std::begin(kVariableKinds)) : 2;
            std::strncpy(varUI.defaultValue, var.defaultValue.c_str(), sizeof(varUI.defaultValue) - 1);
            m_variables.push_back(varUI);
        }
        std::strncpy(m_scriptName, graph->className.c_str(), sizeof(m_scriptName) - 1);
        m_scriptName[sizeof(m_scriptName) - 1] = '\0';
        m_description = graph->description;
        m_functions = std::move(graph->functions);
        m_customEvents = std::move(graph->customEvents);
    }

    // ========================================================================
    // Connection Management
    // ========================================================================

    void VisualScriptPanel::TryStartConnection(int nodeIndex, int pinIndex, bool isOutput)
    {
        m_isDrawingConnection = true;
        m_connectionSourceNode = nodeIndex;
        m_connectionSourcePin = pinIndex;
        m_connectionSourceIsOutput = isOutput;
    }

    void VisualScriptPanel::TryCompleteConnection(int nodeIndex, int pinIndex, bool isOutput)
    {
        if (!m_isDrawingConnection || m_connectionSourceNode < 0)
        {
            m_isDrawingConnection = false;
            return;
        }

        // Must connect output → input (or input → output)
        if (m_connectionSourceIsOutput == isOutput)
        {
            m_isDrawingConnection = false;
            m_connectionSourceNode = -1;
            return;
        }

        // No self-connections
        if (m_connectionSourceNode == nodeIndex)
        {
            m_isDrawingConnection = false;
            m_connectionSourceNode = -1;
            return;
        }

        // Determine which is output and which is input
        int outNode = m_connectionSourceIsOutput ? m_connectionSourceNode : nodeIndex;
        int outPin = m_connectionSourceIsOutput ? m_connectionSourcePin : pinIndex;
        int inNode = m_connectionSourceIsOutput ? nodeIndex : m_connectionSourceNode;
        int inPin = m_connectionSourceIsOutput ? pinIndex : m_connectionSourcePin;

        // Type compatibility check
        if (outNode >= 0 && outNode < static_cast<int>(m_nodes.size()) && inNode >= 0 &&
            inNode < static_cast<int>(m_nodes.size()))
        {
            const auto& srcNode = m_nodes[outNode];
            const auto& dstNode = m_nodes[inNode];

            if (outPin < static_cast<int>(srcNode.node.outputs.size()) &&
                inPin < static_cast<int>(dstNode.node.inputs.size()))
            {
                PinKind srcKind = srcNode.node.outputs[outPin].kind;
                PinKind dstKind = dstNode.node.inputs[inPin].kind;

                if (AreTypesCompatible(srcKind, dstKind))
                {
                    // Check for duplicate connections
                    uint32_t fromId = srcNode.node.id;
                    uint32_t toId = dstNode.node.id;
                    bool duplicate = false;
                    for (const auto& c : m_connections)
                    {
                        if (c.connection.fromNode == fromId && c.connection.fromPin == static_cast<uint32_t>(outPin) &&
                            c.connection.toNode == toId && c.connection.toPin == static_cast<uint32_t>(inPin))
                        {
                            duplicate = true;
                            break;
                        }
                    }

                    if (!duplicate)
                    {
                        // One wire per input, and one wire out of an execution output: the same rules the
                        // .vscript loader enforces, so every graph the panel builds can be saved.
                        const bool execOutput = srcKind == PinKind::Execution;
                        std::erase_if(m_connections,
                                      [fromId, outPin, toId, inPin, execOutput](const ConnectionUI& c)
                                      {
                                          const bool sameInput = c.connection.toNode == toId &&
                                                                 c.connection.toPin == static_cast<uint32_t>(inPin);
                                          const bool sameExecOutput =
                                              execOutput && c.connection.fromNode == fromId &&
                                              c.connection.fromPin == static_cast<uint32_t>(outPin);
                                          return sameInput || sameExecOutput;
                                      });

                        ConnectionUI conn;
                        conn.connection.fromNode = fromId;
                        conn.connection.fromPin = static_cast<uint32_t>(outPin);
                        conn.connection.toNode = toId;
                        conn.connection.toPin = static_cast<uint32_t>(inPin);
                        m_connections.push_back(conn);
                    }
                }
            }
        }

        m_isDrawingConnection = false;
        m_connectionSourceNode = -1;
    }

    bool VisualScriptPanel::AreTypesCompatible(PinKind a, PinKind b) const
    {
        // Same rule the .vscript loader enforces, in either wiring direction.
        return VisualScriptGraphIO::ArePinKindsCompatible(a, b) || VisualScriptGraphIO::ArePinKindsCompatible(b, a);
    }

    void VisualScriptPanel::GetPinScreenPos(int nodeIndex, int pinIndex, bool isOutput, float& outX, float& outY) const
    {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_nodes.size()))
        {
            outX = 0;
            outY = 0;
            return;
        }

        const auto& n = m_nodes[nodeIndex];
        float nx = m_canvasOriginX + (n.posX + m_canvasOffsetX) * m_canvasZoom;
        float ny = m_canvasOriginY + (n.posY + m_canvasOffsetY) * m_canvasZoom;
        float nw = n.width * m_canvasZoom;
        float pinY = ny + (30.0f + pinIndex * 18.0f) * m_canvasZoom;

        outX = isOutput ? (nx + nw) : nx;
        outY = pinY;
    }

    void VisualScriptPanel::RenderPendingConnection()
    {
        if (!m_isDrawingConnection || m_connectionSourceNode < 0)
            return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float startX, startY;
        GetPinScreenPos(m_connectionSourceNode, m_connectionSourcePin, m_connectionSourceIsOutput, startX, startY);
        ImVec2 startPos(startX, startY);
        ImVec2 endPos = ImGui::GetIO().MousePos;

        // Determine wire color from source pin
        ImU32 wireColor = IM_COL32(200, 200, 200, 150);
        if (m_connectionSourceNode < static_cast<int>(m_nodes.size()))
        {
            const auto& srcNode = m_nodes[m_connectionSourceNode];
            if (m_connectionSourceIsOutput && m_connectionSourcePin < static_cast<int>(srcNode.node.outputs.size()))
            {
                wireColor = GetPinColor(srcNode.node.outputs[m_connectionSourcePin].kind);
            }
            else if (!m_connectionSourceIsOutput &&
                     m_connectionSourcePin < static_cast<int>(srcNode.node.inputs.size()))
            {
                wireColor = GetPinColor(srcNode.node.inputs[m_connectionSourcePin].kind);
            }
        }

        float dx = std::abs(endPos.x - startPos.x) * 0.5f;
        if (m_connectionSourceIsOutput)
        {
            drawList->AddBezierCubic(startPos, ImVec2(startPos.x + dx, startPos.y), ImVec2(endPos.x - dx, endPos.y),
                                     endPos, wireColor, 2.0f);
        }
        else
        {
            drawList->AddBezierCubic(startPos, ImVec2(startPos.x - dx, startPos.y), ImVec2(endPos.x + dx, endPos.y),
                                     endPos, wireColor, 2.0f);
        }
    }

    int VisualScriptPanel::HitTestPin(float mouseX, float mouseY, int& outPinIndex, bool& outIsOutput) const
    {
        float hitRadius = 8.0f * m_canvasZoom;

        for (int i = 0; i < static_cast<int>(m_nodes.size()); i++)
        {
            // Check output pins
            for (int p = 0; p < static_cast<int>(m_nodes[i].node.outputs.size()); p++)
            {
                float px, py;
                GetPinScreenPos(i, p, true, px, py);
                float dx = mouseX - px;
                float dy = mouseY - py;
                if (dx * dx + dy * dy < hitRadius * hitRadius)
                {
                    outPinIndex = p;
                    outIsOutput = true;
                    return i;
                }
            }
            // Check input pins
            for (int p = 0; p < static_cast<int>(m_nodes[i].node.inputs.size()); p++)
            {
                float px, py;
                GetPinScreenPos(i, p, false, px, py);
                float dx = mouseX - px;
                float dy = mouseY - py;
                if (dx * dx + dy * dy < hitRadius * hitRadius)
                {
                    outPinIndex = p;
                    outIsOutput = false;
                    return i;
                }
            }
        }
        return -1;
    }

    // ========================================================================
    // Undo/Redo Command Implementations
    // ========================================================================

    void AddNodeCommand::Execute()
    {
        m_panel->AddNodeAtPositionDirect(m_type, m_x, m_y);
        if (!m_panel->GetNodes().empty())
            m_createdNodeId = m_panel->GetNodes().back().node.id;
    }

    void AddNodeCommand::Undo()
    {
        auto& nodes = m_panel->GetNodes();
        for (size_t i = 0; i < nodes.size(); i++)
        {
            if (nodes[i].node.id == m_createdNodeId)
            {
                auto& conns = m_panel->GetConnections();
                std::erase_if(
                    conns, [this](const VisualScriptPanel::ConnectionUI& c)
                    { return c.connection.fromNode == m_createdNodeId || c.connection.toNode == m_createdNodeId; });
                nodes.erase(nodes.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
    }

    void RemoveNodeCommand::Execute()
    {
        auto& nodes = m_panel->GetNodes();
        if (m_nodeIndex >= 0 && m_nodeIndex < static_cast<int>(nodes.size()))
        {
            m_savedNode = nodes[m_nodeIndex].node;
            m_savedX = nodes[m_nodeIndex].posX;
            m_savedY = nodes[m_nodeIndex].posY;

            uint32_t nodeId = m_savedNode.id;
            m_savedConnections.clear();
            for (const auto& c : m_panel->GetConnections())
            {
                if (c.connection.fromNode == nodeId || c.connection.toNode == nodeId)
                    m_savedConnections.push_back(c.connection);
            }

            m_panel->RemoveNodeDirect(m_nodeIndex);
        }
    }

    void RemoveNodeCommand::Undo()
    {
        m_panel->AddNodeAtPositionDirect(m_savedNode.type, m_savedX, m_savedY);
        auto& nodes = m_panel->GetNodes();
        if (!nodes.empty())
            nodes.back().node = m_savedNode;

        for (const auto& conn : m_savedConnections)
            m_panel->AddConnectionDirect(conn);
    }

    void AddConnectionCommand::Execute()
    {
        m_panel->AddConnectionDirect(m_conn);
    }

    void AddConnectionCommand::Undo()
    {
        m_panel->RemoveConnectionDirect(m_conn);
    }

    void VisualScriptPanel::AddNodeAtPositionDirect(ScriptNodeType type, float x, float y)
    {
        AddNodeAtPosition(type, x, y);
    }

    void VisualScriptPanel::RemoveNodeDirect(int nodeIndex)
    {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_nodes.size()))
            return;
        uint32_t nodeId = m_nodes[nodeIndex].node.id;
        std::erase_if(m_connections, [nodeId](const ConnectionUI& c)
                      { return c.connection.fromNode == nodeId || c.connection.toNode == nodeId; });
        m_nodes.erase(m_nodes.begin() + nodeIndex);
        if (m_selectedNode == nodeIndex)
            m_selectedNode = -1;
    }

    void VisualScriptPanel::AddConnectionDirect(const ScriptConnection& conn)
    {
        ConnectionUI c;
        c.connection = conn;
        m_connections.push_back(c);
    }

    void VisualScriptPanel::RemoveConnectionDirect(const ScriptConnection& conn)
    {
        std::erase_if(m_connections,
                      [&conn](const ConnectionUI& c)
                      {
                          return c.connection.fromNode == conn.fromNode && c.connection.fromPin == conn.fromPin &&
                                 c.connection.toNode == conn.toNode && c.connection.toPin == conn.toPin;
                      });
    }

} // namespace SparkEditor
