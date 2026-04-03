/**
 * @file VisualScriptPanel.cpp
 * @brief Implementation of the node-based visual scripting editor
 */

#include "VisualScriptPanel.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace SparkEditor
{

    using namespace Spark::Scripting;

    // Node category definitions for the palette
    struct NodePaletteEntry
    {
        const char* name;
        ScriptNodeType type;
    };

    struct NodeCategory
    {
        const char* name;
        const NodePaletteEntry* entries;
        int count;
    };

    static constexpr NodePaletteEntry kEventNodes[] = {
        {"On Start", ScriptNodeType::OnStart},
        {"On Update", ScriptNodeType::OnUpdate},
        {"On Trigger Enter", ScriptNodeType::OnTriggerEnter},
        {"On Trigger Exit", ScriptNodeType::OnTriggerExit},
        {"On Damaged", ScriptNodeType::OnDamaged},
        {"On Key Press", ScriptNodeType::OnKeyPress},
        {"On Collision", ScriptNodeType::OnCollision},
        {"On Custom Event", ScriptNodeType::OnCustomEvent},
    };

    static constexpr NodePaletteEntry kFlowNodes[] = {
        {"Branch (If)", ScriptNodeType::Branch},
        {"For Loop", ScriptNodeType::ForLoop},
        {"Sequence", ScriptNodeType::Sequence},
    };

    static constexpr NodePaletteEntry kActionNodes[] = {
        {"Set Position", ScriptNodeType::SetPosition},     {"Set Health", ScriptNodeType::SetHealth},
        {"Apply Force", ScriptNodeType::ApplyForce},       {"Play Sound", ScriptNodeType::PlaySound},
        {"Play Animation", ScriptNodeType::PlayAnimation}, {"Spawn Entity", ScriptNodeType::SpawnEntity},
        {"Destroy Entity", ScriptNodeType::DestroyEntity}, {"Print Message", ScriptNodeType::PrintMessage},
        {"Fire Event", ScriptNodeType::FireEvent},
    };

    static constexpr NodePaletteEntry kMathNodes[] = {
        {"Add", ScriptNodeType::Add},           {"Subtract", ScriptNodeType::Subtract},
        {"Multiply", ScriptNodeType::Multiply}, {"Divide", ScriptNodeType::Divide},
        {"Negate", ScriptNodeType::Negate},     {"Abs", ScriptNodeType::Abs},
        {"Lerp", ScriptNodeType::Lerp},         {"Clamp", ScriptNodeType::Clamp},
        {"Random", ScriptNodeType::Random},     {"Random Range", ScriptNodeType::RandomRange},
    };

    static constexpr NodePaletteEntry kLogicNodes[] = {
        {"AND", ScriptNodeType::And},
        {"OR", ScriptNodeType::Or},
        {"NOT", ScriptNodeType::Not},
        {"Equal", ScriptNodeType::Equal},
        {"Not Equal", ScriptNodeType::NotEqual},
        {"Greater", ScriptNodeType::Greater},
        {"Less", ScriptNodeType::Less},
    };

    static constexpr NodePaletteEntry kGetterNodes[] = {
        {"Get Key Down", ScriptNodeType::GetKeyDown},
        {"Get Key Held", ScriptNodeType::GetKey},
        {"Get Delta Time", ScriptNodeType::GetDeltaTime},
        {"Get Self", ScriptNodeType::GetSelf},
    };

    static constexpr NodePaletteEntry kConstantNodes[] = {
        {"Float", ScriptNodeType::ConstFloat},     {"Int", ScriptNodeType::ConstInt},
        {"Bool", ScriptNodeType::ConstBool},       {"String", ScriptNodeType::ConstString},
        {"Vector3", ScriptNodeType::ConstVector3},
    };

    static constexpr NodePaletteEntry kVariableNodes[] = {
        {"Get Variable", ScriptNodeType::GetVariable},
        {"Set Variable", ScriptNodeType::SetVariable},
    };

    static constexpr NodePaletteEntry kFunctionNodes[] = {
        {"Custom Event", ScriptNodeType::DefineCustomEvent},
        {"Call Function", ScriptNodeType::CallFunction},
        {"Return Value", ScriptNodeType::ReturnValue},
    };

    static constexpr NodeCategory kCategories[] = {
        {"Events", kEventNodes, static_cast<int>(std::size(kEventNodes))},
        {"Flow Control", kFlowNodes, static_cast<int>(std::size(kFlowNodes))},
        {"Actions", kActionNodes, static_cast<int>(std::size(kActionNodes))},
        {"Math", kMathNodes, static_cast<int>(std::size(kMathNodes))},
        {"Logic", kLogicNodes, static_cast<int>(std::size(kLogicNodes))},
        {"Getters", kGetterNodes, static_cast<int>(std::size(kGetterNodes))},
        {"Constants", kConstantNodes, static_cast<int>(std::size(kConstantNodes))},
        {"Variables", kVariableNodes, static_cast<int>(std::size(kVariableNodes))},
        {"Functions", kFunctionNodes, static_cast<int>(std::size(kFunctionNodes))},
    };

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
        if (val >= 200 && val <= 212)
            return IM_COL32(80, 80, 140, 255); // Math: purple
        if (val >= 250 && val <= 258)
            return IM_COL32(100, 140, 100, 255); // Logic: teal
        if (val >= 300 && val <= 301)
            return IM_COL32(140, 140, 40, 255); // Variables: yellow
        if (val >= 350 && val <= 354)
            return IM_COL32(100, 100, 100, 255); // Constants: gray
        return IM_COL32(80, 80, 80, 255);
    }

    static const char* GetNodeTitle(ScriptNodeType type)
    {
        // Search categories for display name
        for (const auto& cat : kCategories)
        {
            for (int i = 0; i < cat.count; ++i)
            {
                if (cat.entries[i].type == type)
                {
                    return cat.entries[i].name;
                }
            }
        }
        return "Unknown";
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
        ImGui::Separator();

        for (const auto& category : kCategories)
        {
            if (ImGui::TreeNode(category.name))
            {
                for (int i = 0; i < category.count; ++i)
                {
                    if (ImGui::Selectable(category.entries[i].name))
                    {
                        // Add node at center of canvas view
                        float cx = -m_canvasOffsetX + 300.0f;
                        float cy = -m_canvasOffsetY + 200.0f;
                        AddNodeAtPosition(category.entries[i].type, cx, cy);
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
        m_canvasOrigin = canvasPos;

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

        // Input pins (with click detection for connection dragging)
        float pinRadius = 5.0f * m_canvasZoom;
        float pinY = ny + 30.0f * m_canvasZoom;
        ImGuiIO& pinIO = ImGui::GetIO();
        for (size_t p = 0; p < nodeUI.node.inputs.size(); ++p)
        {
            ImVec2 pinPos(nx, pinY);
            ImU32 pinColor = GetPinColor(nodeUI.node.inputs[p].kind);
            drawList->AddCircleFilled(pinPos, pinRadius, pinColor);

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
        for (const auto& category : kCategories)
        {
            if (ImGui::BeginMenu(category.name))
            {
                for (int i = 0; i < category.count; ++i)
                {
                    if (ImGui::MenuItem(category.entries[i].name))
                    {
                        AddNodeAtPosition(category.entries[i].type, m_contextMenuX, m_contextMenuY);
                    }
                }
                ImGui::EndMenu();
            }
        }
    }

    void VisualScriptPanel::AddNodeAtPosition(ScriptNodeType type, float x, float y)
    {
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

        // Logic
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

        static const char* typeNames[] = {"Bool", "Int", "Float", "String", "Vector3"};

        for (int i = 0; i < static_cast<int>(m_variables.size()); ++i)
        {
            ImGui::PushID(i);
            auto& var = m_variables[i];

            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputText("##name", var.name, sizeof(var.name));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.0f);
            ImGui::Combo("##type", &var.typeIndex, typeNames, 5);
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
            VariableUI var{};
            std::snprintf(var.name, sizeof(var.name), "var%zu", m_variables.size());
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
                int val = static_cast<int>(pin.defaultValue[0]);
                if (ImGui::DragInt("##val", &val))
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

    void VisualScriptPanel::CompileGraph()
    {
        // Build the graph from UI state
        VisualScriptGraph graph;
        graph.className = m_scriptName;

        for (const auto& nodeUI : m_nodes)
        {
            graph.nodes.push_back(nodeUI.node);
        }
        for (const auto& connUI : m_connections)
        {
            graph.connections.push_back(connUI.connection);
        }

        // Map variable types
        static constexpr PinKind kVarTypes[] = {PinKind::Bool, PinKind::Int, PinKind::Float, PinKind::String,
                                                PinKind::Vector3};
        for (const auto& varUI : m_variables)
        {
            VariableDecl var;
            var.name = varUI.name;
            var.type = kVarTypes[std::clamp(varUI.typeIndex, 0, 4)];
            var.defaultValue = varUI.defaultValue;
            graph.variables.push_back(std::move(var));
        }

        auto result = VisualScriptCompiler::Compile(graph);
        m_compileErrors = result.errors;
        m_compileSuccess = result.success;
        m_lastCompiledSource = result.angelScriptSource;

        if (result.success)
        {
            // Write generated .as file
            std::string outPath = std::string(m_savePath) + std::string(m_scriptName) + ".as";
            std::ofstream file(outPath);
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
        std::ofstream file(path);
        if (!file.is_open())
            return;

        // Simple JSON serialization
        file << "{\n";
        file << "  \"className\": \"" << m_scriptName << "\",\n";

        // Nodes
        file << "  \"nodes\": [\n";
        for (size_t i = 0; i < m_nodes.size(); i++)
        {
            const auto& n = m_nodes[i];
            file << "    {\"id\":" << n.node.id << ",\"type\":" << static_cast<uint32_t>(n.node.type)
                 << ",\"x\":" << n.posX << ",\"y\":" << n.posY << ",\"inputs\":" << n.node.inputs.size()
                 << ",\"outputs\":" << n.node.outputs.size();
            // Save node properties
            if (!n.node.properties.empty())
            {
                file << ",\"props\":{";
                bool firstProp = true;
                for (const auto& [key, val] : n.node.properties)
                {
                    if (!firstProp)
                        file << ",";
                    file << "\"" << key << "\":\"" << val << "\"";
                    firstProp = false;
                }
                file << "}";
            }
            // Save pin default values for constants
            if (!n.node.outputs.empty() && static_cast<uint32_t>(n.node.type) >= 350)
            {
                const auto& pin = n.node.outputs[0];
                file << ",\"defVal\":[" << pin.defaultValue[0] << "," << pin.defaultValue[1] << ","
                     << pin.defaultValue[2] << "," << pin.defaultValue[3] << "]";
                if (!pin.defaultString.empty())
                    file << ",\"defStr\":\"" << pin.defaultString << "\"";
            }
            file << "}";
            if (i + 1 < m_nodes.size())
                file << ",";
            file << "\n";
        }
        file << "  ],\n";

        // Connections
        file << "  \"connections\": [\n";
        for (size_t i = 0; i < m_connections.size(); i++)
        {
            const auto& c = m_connections[i].connection;
            file << "    {\"from\":" << c.fromNode << ",\"fromPin\":" << c.fromPin << ",\"to\":" << c.toNode
                 << ",\"toPin\":" << c.toPin << "}";
            if (i + 1 < m_connections.size())
                file << ",";
            file << "\n";
        }
        file << "  ],\n";

        // Variables
        file << "  \"variables\": [\n";
        for (size_t i = 0; i < m_variables.size(); i++)
        {
            const auto& v = m_variables[i];
            file << "    {\"name\":\"" << v.name << "\",\"type\":" << v.typeIndex << ",\"default\":\"" << v.defaultValue
                 << "\"}";
            if (i + 1 < m_variables.size())
                file << ",";
            file << "\n";
        }
        file << "  ]\n";

        file << "}\n";
        file.close();
    }

    void VisualScriptPanel::LoadGraph(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
            return;

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        m_nodes.clear();
        m_connections.clear();
        m_variables.clear();
        m_selectedNode = -1;
        m_nextNodeId = 1;

        // Helpers for parsing our simple JSON format
        auto extractInt = [](const std::string& s, const std::string& key) -> int
        {
            auto pos = s.find("\"" + key + "\":");
            if (pos == std::string::npos)
                return 0;
            pos += key.size() + 3;
            return std::atoi(s.c_str() + pos);
        };
        auto extractFloat = [](const std::string& s, const std::string& key) -> float
        {
            auto pos = s.find("\"" + key + "\":");
            if (pos == std::string::npos)
                return 0.0f;
            pos += key.size() + 3;
            return std::strtof(s.c_str() + pos, nullptr);
        };
        auto extractStr = [](const std::string& s, const std::string& key) -> std::string
        {
            auto pos = s.find("\"" + key + "\":\"");
            if (pos == std::string::npos)
                return "";
            pos += key.size() + 4;
            auto end = s.find("\"", pos);
            return (end != std::string::npos) ? s.substr(pos, end - pos) : "";
        };

        // Parse nodes
        auto parseSection = [&](const std::string& sectionName, auto callback)
        {
            auto start = content.find("\"" + sectionName + "\"");
            auto end = content.find("]", start);
            if (start == std::string::npos || end == std::string::npos)
                return;
            std::string section = content.substr(start, end - start);
            size_t pos = 0;
            while ((pos = section.find("{", pos)) != std::string::npos)
            {
                auto objEnd = section.find("}", pos);
                if (objEnd == std::string::npos)
                    break;
                callback(section.substr(pos, objEnd - pos + 1));
                pos = objEnd + 1;
            }
        };

        parseSection("nodes",
                     [&](const std::string& obj)
                     {
                         uint32_t id = static_cast<uint32_t>(extractInt(obj, "id"));
                         auto type = static_cast<ScriptNodeType>(extractInt(obj, "type"));
                         float x = extractFloat(obj, "x");
                         float y = extractFloat(obj, "y");
                         AddNodeAtPosition(type, x, y);
                         if (!m_nodes.empty())
                         {
                             m_nodes.back().node.id = id;
                             if (id >= m_nextNodeId)
                                 m_nextNodeId = id + 1;
                         }
                     });

        parseSection("connections",
                     [&](const std::string& obj)
                     {
                         ConnectionUI conn;
                         conn.connection.fromNode = static_cast<uint32_t>(extractInt(obj, "from"));
                         conn.connection.fromPin = static_cast<uint32_t>(extractInt(obj, "fromPin"));
                         conn.connection.toNode = static_cast<uint32_t>(extractInt(obj, "to"));
                         conn.connection.toPin = static_cast<uint32_t>(extractInt(obj, "toPin"));
                         m_connections.push_back(conn);
                     });

        parseSection("variables",
                     [&](const std::string& obj)
                     {
                         VariableUI var{};
                         std::string name = extractStr(obj, "name");
                         std::strncpy(var.name, name.c_str(), sizeof(var.name) - 1);
                         var.typeIndex = extractInt(obj, "type");
                         std::string defVal = extractStr(obj, "default");
                         std::strncpy(var.defaultValue, defVal.c_str(), sizeof(var.defaultValue) - 1);
                         m_variables.push_back(var);
                     });

        std::string className = extractStr(content, "className");
        if (!className.empty())
            std::strncpy(m_scriptName, className.c_str(), sizeof(m_scriptName) - 1);
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
                        // Remove existing connection to same input (only one wire per input)
                        std::erase_if(m_connections,
                                      [toId, inPin](const ConnectionUI& c) {
                                          return c.connection.toNode == toId &&
                                                 c.connection.toPin == static_cast<uint32_t>(inPin);
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
        // Execution pins can only connect to Execution pins
        if (a == PinKind::Execution || b == PinKind::Execution)
            return a == b;
        // Data pins
        if (a == b)
            return true;
        if (a == PinKind::Any || b == PinKind::Any)
            return true;
        // Allow Int ↔ Float implicit conversion
        if ((a == PinKind::Int && b == PinKind::Float) || (a == PinKind::Float && b == PinKind::Int))
            return true;
        return false;
    }

    ImVec2 VisualScriptPanel::GetPinScreenPos(int nodeIndex, int pinIndex, bool isOutput) const
    {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(m_nodes.size()))
            return ImVec2(0, 0);

        const auto& n = m_nodes[nodeIndex];
        float nx = m_canvasOrigin.x + (n.posX + m_canvasOffsetX) * m_canvasZoom;
        float ny = m_canvasOrigin.y + (n.posY + m_canvasOffsetY) * m_canvasZoom;
        float nw = n.width * m_canvasZoom;
        float pinY = ny + (30.0f + pinIndex * 18.0f) * m_canvasZoom;

        if (isOutput)
            return ImVec2(nx + nw, pinY);
        return ImVec2(nx, pinY);
    }

    void VisualScriptPanel::RenderPendingConnection()
    {
        if (!m_isDrawingConnection || m_connectionSourceNode < 0)
            return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 startPos = GetPinScreenPos(m_connectionSourceNode, m_connectionSourcePin, m_connectionSourceIsOutput);
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
                ImVec2 pos = GetPinScreenPos(i, p, true);
                float dx = mouseX - pos.x;
                float dy = mouseY - pos.y;
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
                ImVec2 pos = GetPinScreenPos(i, p, false);
                float dx = mouseX - pos.x;
                float dy = mouseY - pos.y;
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

} // namespace SparkEditor
