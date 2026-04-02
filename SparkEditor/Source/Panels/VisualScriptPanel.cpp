/**
 * @file VisualScriptPanel.cpp
 * @brief Implementation of the node-based visual scripting editor
 */

#include "VisualScriptPanel.h"
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

    static constexpr NodeCategory kCategories[] = {
        {"Events", kEventNodes, static_cast<int>(std::size(kEventNodes))},
        {"Flow Control", kFlowNodes, static_cast<int>(std::size(kFlowNodes))},
        {"Actions", kActionNodes, static_cast<int>(std::size(kActionNodes))},
        {"Math", kMathNodes, static_cast<int>(std::size(kMathNodes))},
        {"Logic", kLogicNodes, static_cast<int>(std::size(kLogicNodes))},
        {"Getters", kGetterNodes, static_cast<int>(std::size(kGetterNodes))},
        {"Constants", kConstantNodes, static_cast<int>(std::size(kConstantNodes))},
        {"Variables", kVariableNodes, static_cast<int>(std::size(kVariableNodes))},
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

        // Draw connections
        RenderConnections();

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

        // Input pins
        float pinY = ny + 30.0f * m_canvasZoom;
        for (size_t p = 0; p < nodeUI.node.inputs.size(); ++p)
        {
            ImU32 pinColor = GetPinColor(nodeUI.node.inputs[p].kind);
            drawList->AddCircleFilled(ImVec2(nx, pinY), 5.0f * m_canvasZoom, pinColor);
            pinY += 18.0f * m_canvasZoom;
        }

        // Output pins
        pinY = ny + 30.0f * m_canvasZoom;
        for (size_t p = 0; p < nodeUI.node.outputs.size(); ++p)
        {
            ImU32 pinColor = GetPinColor(nodeUI.node.outputs[p].kind);
            drawList->AddCircleFilled(ImVec2(nx + nw, pinY), 5.0f * m_canvasZoom, pinColor);
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

        // Click on empty space deselects
        if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered())
        {
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

        auto val = static_cast<uint32_t>(type);

        // Events have execution output
        if (val <= 7)
        {
            addOutput(PinKind::Execution);
        }

        // Math nodes: two float inputs, one float output
        if (val >= 200 && val <= 210)
        {
            addInput(PinKind::Float, "", 0.0f);
            if (type != ScriptNodeType::Negate && type != ScriptNodeType::Abs && type != ScriptNodeType::Random)
            {
                addInput(PinKind::Float, "", 0.0f);
            }
            if (type == ScriptNodeType::Lerp || type == ScriptNodeType::Clamp)
            {
                addInput(PinKind::Float, "", 0.0f);
            }
            addOutput(PinKind::Float);
        }

        // Logic nodes
        if (val >= 250 && val <= 258)
        {
            addInput(PinKind::Bool);
            if (type != ScriptNodeType::Not)
            {
                addInput(PinKind::Bool);
            }
            addOutput(PinKind::Bool);
        }

        // Comparison nodes that take floats (Equal through LessEqual)
        if (type == ScriptNodeType::Equal || type == ScriptNodeType::NotEqual || type == ScriptNodeType::Greater ||
            type == ScriptNodeType::Less || type == ScriptNodeType::GreaterEqual || type == ScriptNodeType::LessEqual)
        {
            // Override: inputs should be float for comparison
            nodeUI.node.inputs.clear();
            addInput(PinKind::Float);
            addInput(PinKind::Float);
            addOutput(PinKind::Bool);
        }

        // Getters
        if (type == ScriptNodeType::GetKeyDown || type == ScriptNodeType::GetKey)
        {
            addOutput(PinKind::Bool);
        }
        if (type == ScriptNodeType::GetDeltaTime)
        {
            addOutput(PinKind::Float);
        }
        if (type == ScriptNodeType::GetSelf)
        {
            addOutput(PinKind::Entity);
        }

        // Action nodes: execution input + specific data inputs
        if (type == ScriptNodeType::PrintMessage)
        {
            addInput(PinKind::String, "Hello!");
            addOutput(PinKind::Execution);
        }
        if (type == ScriptNodeType::SpawnEntity)
        {
            addInput(PinKind::String, "Entity");
            addOutput(PinKind::Entity);
        }
        if (type == ScriptNodeType::Branch)
        {
            addInput(PinKind::Bool);
            addOutput(PinKind::Execution); // True
            addOutput(PinKind::Execution); // False
        }

        // Constants
        if (type == ScriptNodeType::ConstFloat)
        {
            addOutput(PinKind::Float);
        }
        if (type == ScriptNodeType::ConstInt)
        {
            addOutput(PinKind::Int);
        }
        if (type == ScriptNodeType::ConstBool)
        {
            addOutput(PinKind::Bool);
        }
        if (type == ScriptNodeType::ConstString)
        {
            addOutput(PinKind::String);
        }
        if (type == ScriptNodeType::ConstVector3)
        {
            addOutput(PinKind::Vector3);
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
        if (nodeUI.node.type == ScriptNodeType::OnKeyPress || nodeUI.node.type == ScriptNodeType::GetKeyDown ||
            nodeUI.node.type == ScriptNodeType::GetKey)
        {
            auto& props = nodeUI.node.properties;
            char keyBuf[64];
            auto it = props.find("key");
            std::strncpy(keyBuf, (it != props.end()) ? it->second.c_str() : "Space", sizeof(keyBuf) - 1);
            keyBuf[sizeof(keyBuf) - 1] = '\0';
            if (ImGui::InputText("Key", keyBuf, sizeof(keyBuf)))
            {
                props["key"] = keyBuf;
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
        }
    }

    void VisualScriptPanel::SaveGraph(const std::string& /*path*/)
    {
        // Graph serialization to .vscript JSON — future enhancement
    }

    void VisualScriptPanel::LoadGraph(const std::string& /*path*/)
    {
        // Graph deserialization from .vscript JSON — future enhancement
    }

} // namespace SparkEditor
