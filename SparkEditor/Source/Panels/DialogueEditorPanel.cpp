/**
 * @file DialogueEditorPanel.cpp
 * @brief Node-based dialogue tree editor implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "DialogueEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Utils/ImGuiUtils.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace SparkEditor
{

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    DialogueEditorPanel::DialogueEditorPanel() : EditorPanel("Dialogue Editor", "dialogue_editor_panel")
    {
        SetIcon(ICON_FA_COMMENT);
    }

    DialogueEditorPanel::~DialogueEditorPanel() = default;

    // =========================================================================
    // EditorPanel interface
    // =========================================================================

    bool DialogueEditorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Dialogue Editor panel\n";
        NewDialogue();
        m_isInitialized = true;
        return true;
    }

    void DialogueEditorPanel::Update(float /*deltaTime*/) {}

    void DialogueEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderToolbar();
        ImGui::Separator();

        // Main layout: canvas left, inspector right
        float inspectorWidth = 300.0f;
        float availWidth = ImGui::GetContentRegionAvail().x;
        float canvasWidth = availWidth - inspectorWidth - 8.0f;
        if (canvasWidth < 200.0f)
        {
            canvasWidth = availWidth;
            inspectorWidth = 0.0f;
        }

        // Canvas area
        ImGui::BeginChild("DialogueCanvas", ImVec2(canvasWidth, 0), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (m_previewMode)
        {
            RenderPreviewMode();
        }
        else
        {
            RenderCanvas();
        }
        ImGui::EndChild();

        // Inspector sidebar
        if (inspectorWidth > 0.0f)
        {
            ImGui::SameLine();
            ImGui::BeginChild("DialogueInspector", ImVec2(inspectorWidth, 0), true);

            if (ImGui::CollapsingHeader(ICON_FA_SLIDERS " Node Inspector", ImGuiTreeNodeFlags_DefaultOpen))
            {
                RenderNodeInspector();
            }
            if (ImGui::CollapsingHeader(ICON_FA_USER " Characters"))
            {
                RenderCharacterList();
            }
            if (ImGui::CollapsingHeader(ICON_FA_COG " Variables"))
            {
                RenderVariableList();
            }

            ImGui::EndChild();
        }

        // Context menu
        if (m_showContextMenu)
        {
            RenderContextMenu();
        }

        // Minimap overlay
        if (m_showMinimap && !m_previewMode)
        {
            RenderMinimap();
        }

        EndPanel();
    }

    void DialogueEditorPanel::Shutdown()
    {
        m_currentDialogue.reset();
        std::cout << "Dialogue Editor panel shut down\n";
    }

    // =========================================================================
    // Public API
    // =========================================================================

    void DialogueEditorPanel::NewDialogue()
    {
        m_currentDialogue = std::make_unique<DialogueTreeAsset>();
        m_currentDialogue->name = "New Dialogue";

        // Create entry node
        DialogueNode entry;
        entry.id = m_currentDialogue->GenerateNodeId();
        entry.type = DialogueNodeType::Entry;
        entry.title = "Start";
        entry.editorPosition = ImVec2(100, 200);
        m_currentDialogue->entryNodeId = entry.id;
        m_currentDialogue->nodes.push_back(entry);

        // Create a default NPC line
        DialogueNode npcLine;
        npcLine.id = m_currentDialogue->GenerateNodeId();
        npcLine.type = DialogueNodeType::NPCLine;
        npcLine.title = "Greeting";
        npcLine.speakerName = "NPC";
        npcLine.dialogueText = "Hello, traveler!";
        npcLine.editorPosition = ImVec2(350, 200);
        m_currentDialogue->nodes.push_back(npcLine);

        // Connect entry to NPC line
        ConnectNodes(entry.id, npcLine.id, 0);

        m_currentDialogue->characters.push_back("NPC");
        m_currentDialogue->isModified = false;
        m_selectedNodeId = INVALID_DIALOGUE_NODE;
        m_canvasOffset = ImVec2(0, 0);
        m_canvasZoom = 1.0f;
    }

    bool DialogueEditorPanel::LoadDialogue(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            std::cout << "Failed to open dialogue file: " << filePath << "\n";
            return false;
        }

        auto dialogue = std::make_unique<DialogueTreeAsset>();
        dialogue->filePath = filePath;

        std::string line;
        DialogueNode* currentNode = nullptr;

        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            if (line.rfind("NAME:", 0) == 0)
            {
                dialogue->name = line.substr(5);
            }
            else if (line.rfind("ENTRY:", 0) == 0)
            {
                dialogue->entryNodeId = static_cast<DialogueNodeId>(std::stoul(line.substr(6)));
            }
            else if (line.rfind("CHARACTER:", 0) == 0)
            {
                dialogue->characters.push_back(line.substr(10));
            }
            else if (line.rfind("VAR:", 0) == 0)
            {
                auto eqPos = line.find('=', 4);
                if (eqPos != std::string::npos)
                {
                    dialogue->variables[line.substr(4, eqPos - 4)] = line.substr(eqPos + 1);
                }
            }
            else if (line.rfind("NODE:", 0) == 0)
            {
                dialogue->nodes.emplace_back();
                currentNode = &dialogue->nodes.back();
                currentNode->id = static_cast<DialogueNodeId>(std::stoul(line.substr(5)));
                if (currentNode->id >= dialogue->nextNodeId)
                {
                    dialogue->nextNodeId = currentNode->id + 1;
                }
            }
            else if (currentNode)
            {
                if (line.rfind("  TYPE:", 0) == 0)
                {
                    int t = std::stoi(line.substr(7));
                    currentNode->type = static_cast<DialogueNodeType>(t);
                }
                else if (line.rfind("  TITLE:", 0) == 0)
                {
                    currentNode->title = line.substr(8);
                }
                else if (line.rfind("  SPEAKER:", 0) == 0)
                {
                    currentNode->speakerName = line.substr(10);
                }
                else if (line.rfind("  TEXT:", 0) == 0)
                {
                    currentNode->dialogueText = line.substr(7);
                }
                else if (line.rfind("  NEXT:", 0) == 0)
                {
                    currentNode->nextNodeId = static_cast<DialogueNodeId>(std::stoul(line.substr(7)));
                }
                else if (line.rfind("  POS:", 0) == 0)
                {
                    float x = 0, y = 0;
                    if (std::sscanf(line.c_str() + 6, "%f,%f", &x, &y) == 2)
                    {
                        currentNode->editorPosition = ImVec2(x, y);
                    }
                }
            }
        }

        // Rebuild connections from node data
        dialogue->connections.clear();
        for (const auto& node : dialogue->nodes)
        {
            if (node.nextNodeId != INVALID_DIALOGUE_NODE)
            {
                DialogueConnection conn;
                conn.fromNodeId = node.id;
                conn.toNodeId = node.nextNodeId;
                conn.outputIndex = 0;
                dialogue->connections.push_back(conn);
            }
            for (int i = 0; i < static_cast<int>(node.choices.size()); ++i)
            {
                if (node.choices[i].targetNodeId != INVALID_DIALOGUE_NODE)
                {
                    DialogueConnection conn;
                    conn.fromNodeId = node.id;
                    conn.toNodeId = node.choices[i].targetNodeId;
                    conn.outputIndex = i;
                    dialogue->connections.push_back(conn);
                }
            }
            if (node.type == DialogueNodeType::Condition)
            {
                if (node.trueTargetId != INVALID_DIALOGUE_NODE)
                {
                    DialogueConnection conn;
                    conn.fromNodeId = node.id;
                    conn.toNodeId = node.trueTargetId;
                    conn.outputIndex = 0;
                    conn.color = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
                    dialogue->connections.push_back(conn);
                }
                if (node.falseTargetId != INVALID_DIALOGUE_NODE)
                {
                    DialogueConnection conn;
                    conn.fromNodeId = node.id;
                    conn.toNodeId = node.falseTargetId;
                    conn.outputIndex = 1;
                    conn.color = ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
                    dialogue->connections.push_back(conn);
                }
            }
        }

        m_currentDialogue = std::move(dialogue);
        m_currentDialogue->isModified = false;
        m_selectedNodeId = INVALID_DIALOGUE_NODE;
        std::cout << "Loaded dialogue: " << m_currentDialogue->name << " (" << m_currentDialogue->nodes.size()
                  << " nodes)\n";
        return true;
    }

    bool DialogueEditorPanel::SaveDialogue(const std::string& filePath)
    {
        if (!m_currentDialogue)
            return false;

        std::string path = filePath.empty() ? m_currentDialogue->filePath : filePath;
        if (path.empty())
        {
            path = m_currentDialogue->name + ".sparkdlg";
        }

        std::ofstream file(path);
        if (!file.is_open())
        {
            std::cout << "Failed to save dialogue to: " << path << "\n";
            return false;
        }

        file << "# Spark Dialogue File\n";
        file << "NAME:" << m_currentDialogue->name << "\n";
        file << "ENTRY:" << m_currentDialogue->entryNodeId << "\n";

        for (const auto& character : m_currentDialogue->characters)
        {
            file << "CHARACTER:" << character << "\n";
        }
        for (const auto& [key, value] : m_currentDialogue->variables)
        {
            file << "VAR:" << key << "=" << value << "\n";
        }

        for (const auto& node : m_currentDialogue->nodes)
        {
            file << "NODE:" << node.id << "\n";
            file << "  TYPE:" << static_cast<int>(node.type) << "\n";
            file << "  TITLE:" << node.title << "\n";
            file << "  SPEAKER:" << node.speakerName << "\n";
            file << "  TEXT:" << node.dialogueText << "\n";
            file << "  NEXT:" << node.nextNodeId << "\n";
            file << "  POS:" << node.editorPosition.x << "," << node.editorPosition.y << "\n";
        }

        m_currentDialogue->filePath = path;
        m_currentDialogue->isModified = false;
        std::cout << "Saved dialogue to: " << path << "\n";
        return true;
    }

    DialogueNodeId DialogueEditorPanel::AddNode(DialogueNodeType type, ImVec2 position)
    {
        if (!m_currentDialogue)
            return INVALID_DIALOGUE_NODE;

        DialogueNode node;
        node.id = m_currentDialogue->GenerateNodeId();
        node.type = type;
        node.editorPosition = position;

        switch (type)
        {
        case DialogueNodeType::Entry:
            node.title = "Entry";
            break;
        case DialogueNodeType::NPCLine:
            node.title = "NPC Line";
            node.speakerName = m_currentDialogue->characters.empty() ? "NPC" : m_currentDialogue->characters[0];
            node.dialogueText = "...";
            break;
        case DialogueNodeType::PlayerChoice:
            node.title = "Player Choice";
            {
                DialogueChoice defaultChoice;
                defaultChoice.text = "Option 1";
                defaultChoice.isDefault = true;
                node.choices.push_back(defaultChoice);
            }
            break;
        case DialogueNodeType::Condition:
            node.title = "Condition";
            node.condition.variableName = "variable";
            node.condition.compareValue = "true";
            break;
        case DialogueNodeType::Event:
            node.title = "Event";
            {
                DialogueEvent evt;
                evt.eventName = "event_name";
                node.events.push_back(evt);
            }
            break;
        case DialogueNodeType::SetVariable:
            node.title = "Set Variable";
            node.setVariableName = "variable";
            node.setVariableValue = "value";
            break;
        case DialogueNodeType::Random:
            node.title = "Random";
            break;
        case DialogueNodeType::Delay:
            node.title = "Delay";
            node.delayDuration = 1.0f;
            break;
        case DialogueNodeType::Exit:
            node.title = "Exit";
            break;
        }

        DialogueNodeId id = node.id;
        m_currentDialogue->nodes.push_back(std::move(node));
        m_currentDialogue->isModified = true;
        return id;
    }

    void DialogueEditorPanel::DeleteNode(DialogueNodeId nodeId)
    {
        if (!m_currentDialogue || nodeId == INVALID_DIALOGUE_NODE)
            return;

        // Don't delete entry node
        if (nodeId == m_currentDialogue->entryNodeId)
            return;

        // Remove connections involving this node
        auto& conns = m_currentDialogue->connections;
        conns.erase(std::remove_if(conns.begin(), conns.end(), [nodeId](const DialogueConnection& c)
                                   { return c.fromNodeId == nodeId || c.toNodeId == nodeId; }),
                    conns.end());

        // Clear references in other nodes
        for (auto& node : m_currentDialogue->nodes)
        {
            if (node.nextNodeId == nodeId)
                node.nextNodeId = INVALID_DIALOGUE_NODE;
            if (node.trueTargetId == nodeId)
                node.trueTargetId = INVALID_DIALOGUE_NODE;
            if (node.falseTargetId == nodeId)
                node.falseTargetId = INVALID_DIALOGUE_NODE;
            for (auto& choice : node.choices)
            {
                if (choice.targetNodeId == nodeId)
                    choice.targetNodeId = INVALID_DIALOGUE_NODE;
            }
            auto& rt = node.randomTargets;
            rt.erase(std::remove(rt.begin(), rt.end(), nodeId), rt.end());
        }

        // Remove the node
        auto& nodes = m_currentDialogue->nodes;
        nodes.erase(
            std::remove_if(nodes.begin(), nodes.end(), [nodeId](const DialogueNode& n) { return n.id == nodeId; }),
            nodes.end());

        if (m_selectedNodeId == nodeId)
            m_selectedNodeId = INVALID_DIALOGUE_NODE;

        m_currentDialogue->isModified = true;
    }

    void DialogueEditorPanel::ConnectNodes(DialogueNodeId fromId, DialogueNodeId toId, int outputIndex)
    {
        if (!m_currentDialogue || fromId == INVALID_DIALOGUE_NODE || toId == INVALID_DIALOGUE_NODE)
            return;

        DialogueNode* fromNode = m_currentDialogue->FindNode(fromId);
        if (!fromNode)
            return;

        // Update node data based on type
        switch (fromNode->type)
        {
        case DialogueNodeType::Condition:
            if (outputIndex == 0)
                fromNode->trueTargetId = toId;
            else
                fromNode->falseTargetId = toId;
            break;
        case DialogueNodeType::PlayerChoice:
            if (outputIndex >= 0 && outputIndex < static_cast<int>(fromNode->choices.size()))
                fromNode->choices[outputIndex].targetNodeId = toId;
            break;
        case DialogueNodeType::Random:
            if (std::find(fromNode->randomTargets.begin(), fromNode->randomTargets.end(), toId) ==
                fromNode->randomTargets.end())
            {
                fromNode->randomTargets.push_back(toId);
            }
            break;
        default:
            fromNode->nextNodeId = toId;
            break;
        }

        // Add visual connection
        DialogueConnection conn;
        conn.fromNodeId = fromId;
        conn.toNodeId = toId;
        conn.outputIndex = outputIndex;
        conn.color = GetNodeColor(fromNode->type);
        m_currentDialogue->connections.push_back(conn);
        m_currentDialogue->isModified = true;
    }

    // =========================================================================
    // Render sub-sections
    // =========================================================================

    void DialogueEditorPanel::RenderToolbar()
    {
        if (ImGui::Button(ICON_FA_FILE " New"))
        {
            NewDialogue();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Create new dialogue");

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " Load"))
        {
            // Load placeholder
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Load dialogue from file");

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SAVE " Save"))
        {
            SaveDialogue();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save current dialogue");

        SparkEditor::VerticalSeparator();

        // Add node buttons
        if (ImGui::Button(ICON_FA_PLUS " NPC Line"))
        {
            ImVec2 pos = ScreenToCanvas(ImVec2(m_width * 0.5f, m_height * 0.5f));
            AddNode(DialogueNodeType::NPCLine, pos);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_USER " Choice"))
        {
            ImVec2 pos = ScreenToCanvas(ImVec2(m_width * 0.5f + 50, m_height * 0.5f));
            AddNode(DialogueNodeType::PlayerChoice, pos);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COG " Condition"))
        {
            ImVec2 pos = ScreenToCanvas(ImVec2(m_width * 0.5f + 100, m_height * 0.5f));
            AddNode(DialogueNodeType::Condition, pos);
        }

        SparkEditor::VerticalSeparator();

        // Preview mode toggle
        if (m_previewMode)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
        }
        if (ImGui::Button(m_previewMode ? ICON_FA_STOP " Stop Preview" : ICON_FA_PLAY " Preview"))
        {
            m_previewMode = !m_previewMode;
            if (m_previewMode && m_currentDialogue)
            {
                m_previewCurrentNode = m_currentDialogue->entryNodeId;
            }
        }
        if (m_previewMode)
        {
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Grid", &m_showGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &m_snapToGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Minimap", &m_showMinimap);

        // Dialogue name
        if (m_currentDialogue)
        {
            SparkEditor::VerticalSeparator();

            char nameBuf[256];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_currentDialogue->name.c_str());
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::InputText("##DlgName", nameBuf, sizeof(nameBuf)))
            {
                m_currentDialogue->name = nameBuf;
                m_currentDialogue->isModified = true;
            }

            if (m_currentDialogue->isModified)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "(unsaved)");
            }
        }
    }

    void DialogueEditorPanel::RenderCanvas()
    {
        if (!m_currentDialogue)
            return;

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                IM_COL32(30, 30, 35, 255));

        drawList->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), true);

        // Grid
        if (m_showGrid)
        {
            float gridStep = m_gridSize * m_canvasZoom;
            if (gridStep > 5.0f)
            {
                float offsetX = std::fmod(m_canvasOffset.x * m_canvasZoom, gridStep);
                float offsetY = std::fmod(m_canvasOffset.y * m_canvasZoom, gridStep);

                for (float x = offsetX; x < canvasSize.x; x += gridStep)
                {
                    drawList->AddLine(ImVec2(canvasPos.x + x, canvasPos.y),
                                      ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y), IM_COL32(50, 50, 55, 255));
                }
                for (float y = offsetY; y < canvasSize.y; y += gridStep)
                {
                    drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + y),
                                      ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y), IM_COL32(50, 50, 55, 255));
                }
            }
        }

        // Render connections first (behind nodes)
        RenderConnections();

        // Render in-progress connection
        if (m_isDrawingConnection)
        {
            DialogueNode* srcNode = m_currentDialogue->FindNode(m_connectionSourceId);
            if (srcNode)
            {
                ImVec2 startScreen =
                    CanvasToScreen(ImVec2(srcNode->editorPosition.x + 100.0f, srcNode->editorPosition.y + 30.0f));
                DrawConnectionCurve(startScreen, m_connectionEndPos, ImVec4(1.0f, 1.0f, 0.0f, 0.8f), 2.0f);
            }
        }

        // Render nodes
        m_hoveredNodeId = INVALID_DIALOGUE_NODE;
        for (auto& node : m_currentDialogue->nodes)
        {
            RenderNode(node);
        }

        drawList->PopClipRect();

        // Handle input
        HandleCanvasInput();
    }

    void DialogueEditorPanel::RenderNode(DialogueNode& node)
    {
        switch (node.type)
        {
        case DialogueNodeType::NPCLine:
            RenderNPCLineNode(node);
            break;
        case DialogueNodeType::PlayerChoice:
            RenderPlayerChoiceNode(node);
            break;
        case DialogueNodeType::Condition:
            RenderConditionNode(node);
            break;
        case DialogueNodeType::Event:
            RenderEventNode(node);
            break;
        default:
        {
            // Generic node rendering for Entry, SetVariable, Random, Delay, Exit
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImVec2 nodeScreen = CanvasToScreen(node.editorPosition);
            float nodeWidth = 140.0f * m_canvasZoom;
            float nodeHeight = 50.0f * m_canvasZoom;
            ImVec2 nodeEnd = ImVec2(nodeScreen.x + nodeWidth, nodeScreen.y + nodeHeight);

            ImVec4 color = GetNodeColor(node.type);
            ImU32 bgCol = IM_COL32(40, 40, 48, 230);
            ImU32 borderCol = ImGui::ColorConvertFloat4ToU32(color);
            if (node.id == m_selectedNodeId)
            {
                bgCol = IM_COL32(55, 55, 65, 240);
            }

            drawList->AddRectFilled(nodeScreen, nodeEnd, bgCol, 6.0f * m_canvasZoom);
            drawList->AddRect(nodeScreen, nodeEnd, borderCol, 6.0f * m_canvasZoom, 0, 2.0f);

            // Header bar
            float headerH = 20.0f * m_canvasZoom;
            drawList->AddRectFilled(
                nodeScreen, ImVec2(nodeEnd.x, nodeScreen.y + headerH),
                ImGui::ColorConvertFloat4ToU32(ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, 0.9f)),
                6.0f * m_canvasZoom, ImDrawFlags_RoundCornersTop);

            // Title
            float fontSize = 12.0f * m_canvasZoom;
            if (fontSize > 6.0f)
            {
                const char* typeName = GetNodeTypeName(node.type);
                std::string label = std::string(typeName) + ": " + node.title;
                drawList->AddText(ImVec2(nodeScreen.x + 6.0f * m_canvasZoom, nodeScreen.y + 3.0f * m_canvasZoom),
                                  IM_COL32(220, 220, 220, 255), label.c_str());
            }

            // Hit test
            ImVec2 mousePos = ImGui::GetMousePos();
            if (mousePos.x >= nodeScreen.x && mousePos.x <= nodeEnd.x && mousePos.y >= nodeScreen.y &&
                mousePos.y <= nodeEnd.y)
            {
                m_hoveredNodeId = node.id;
            }
            break;
        }
        }
    }

    void DialogueEditorPanel::RenderNPCLineNode(DialogueNode& node)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 nodeScreen = CanvasToScreen(node.editorPosition);
        float nodeWidth = 180.0f * m_canvasZoom;
        float nodeHeight = 70.0f * m_canvasZoom;
        ImVec2 nodeEnd = ImVec2(nodeScreen.x + nodeWidth, nodeScreen.y + nodeHeight);

        ImVec4 color = GetNodeColor(DialogueNodeType::NPCLine);
        ImU32 bgCol = (node.id == m_selectedNodeId) ? IM_COL32(55, 55, 65, 240) : IM_COL32(40, 40, 48, 230);

        drawList->AddRectFilled(nodeScreen, nodeEnd, bgCol, 6.0f * m_canvasZoom);
        drawList->AddRect(nodeScreen, nodeEnd, ImGui::ColorConvertFloat4ToU32(color), 6.0f * m_canvasZoom, 0, 2.0f);

        // Header
        float headerH = 20.0f * m_canvasZoom;
        drawList->AddRectFilled(
            nodeScreen, ImVec2(nodeEnd.x, nodeScreen.y + headerH),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, 0.9f)),
            6.0f * m_canvasZoom, ImDrawFlags_RoundCornersTop);

        float fontSize = 12.0f * m_canvasZoom;
        if (fontSize > 6.0f)
        {
            std::string header = node.speakerName + " - " + node.title;
            drawList->AddText(ImVec2(nodeScreen.x + 6.0f * m_canvasZoom, nodeScreen.y + 3.0f * m_canvasZoom),
                              IM_COL32(220, 220, 220, 255), header.c_str());

            // Dialogue text preview (truncated)
            std::string preview = node.dialogueText;
            if (preview.length() > 25)
                preview = preview.substr(0, 22) + "...";
            drawList->AddText(ImVec2(nodeScreen.x + 6.0f * m_canvasZoom, nodeScreen.y + headerH + 6.0f * m_canvasZoom),
                              IM_COL32(180, 180, 180, 255), preview.c_str());
        }

        // Output connector dot
        float connY = nodeScreen.y + nodeHeight * 0.5f;
        drawList->AddCircleFilled(ImVec2(nodeEnd.x, connY), 5.0f * m_canvasZoom, IM_COL32(200, 200, 200, 255));

        // Input connector dot
        drawList->AddCircleFilled(ImVec2(nodeScreen.x, connY), 5.0f * m_canvasZoom, IM_COL32(200, 200, 200, 255));

        // Hit test
        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.x >= nodeScreen.x && mousePos.x <= nodeEnd.x && mousePos.y >= nodeScreen.y &&
            mousePos.y <= nodeEnd.y)
        {
            m_hoveredNodeId = node.id;
        }
    }

    void DialogueEditorPanel::RenderPlayerChoiceNode(DialogueNode& node)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 nodeScreen = CanvasToScreen(node.editorPosition);
        int numChoices = std::max(1, static_cast<int>(node.choices.size()));
        float nodeWidth = 180.0f * m_canvasZoom;
        float nodeHeight = (30.0f + numChoices * 18.0f) * m_canvasZoom;
        ImVec2 nodeEnd = ImVec2(nodeScreen.x + nodeWidth, nodeScreen.y + nodeHeight);

        ImVec4 color = GetNodeColor(DialogueNodeType::PlayerChoice);
        ImU32 bgCol = (node.id == m_selectedNodeId) ? IM_COL32(55, 55, 65, 240) : IM_COL32(40, 40, 48, 230);

        drawList->AddRectFilled(nodeScreen, nodeEnd, bgCol, 6.0f * m_canvasZoom);
        drawList->AddRect(nodeScreen, nodeEnd, ImGui::ColorConvertFloat4ToU32(color), 6.0f * m_canvasZoom, 0, 2.0f);

        float headerH = 20.0f * m_canvasZoom;
        drawList->AddRectFilled(
            nodeScreen, ImVec2(nodeEnd.x, nodeScreen.y + headerH),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, 0.9f)),
            6.0f * m_canvasZoom, ImDrawFlags_RoundCornersTop);

        float fontSize = 12.0f * m_canvasZoom;
        if (fontSize > 6.0f)
        {
            drawList->AddText(ImVec2(nodeScreen.x + 6.0f * m_canvasZoom, nodeScreen.y + 3.0f * m_canvasZoom),
                              IM_COL32(220, 220, 220, 255), ("Player: " + node.title).c_str());

            for (int i = 0; i < static_cast<int>(node.choices.size()); ++i)
            {
                float yOff = headerH + (6.0f + i * 18.0f) * m_canvasZoom;
                std::string choiceText = std::to_string(i + 1) + ". " + node.choices[i].text;
                if (choiceText.length() > 22)
                    choiceText = choiceText.substr(0, 19) + "...";
                drawList->AddText(ImVec2(nodeScreen.x + 8.0f * m_canvasZoom, nodeScreen.y + yOff),
                                  IM_COL32(180, 220, 180, 255), choiceText.c_str());

                // Output connector per choice
                float connY = nodeScreen.y + yOff + 6.0f * m_canvasZoom;
                drawList->AddCircleFilled(ImVec2(nodeEnd.x, connY), 4.0f * m_canvasZoom, IM_COL32(200, 200, 100, 255));
            }
        }

        // Input connector
        drawList->AddCircleFilled(ImVec2(nodeScreen.x, nodeScreen.y + nodeHeight * 0.5f), 5.0f * m_canvasZoom,
                                  IM_COL32(200, 200, 200, 255));

        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.x >= nodeScreen.x && mousePos.x <= nodeEnd.x && mousePos.y >= nodeScreen.y &&
            mousePos.y <= nodeEnd.y)
        {
            m_hoveredNodeId = node.id;
        }
    }

    void DialogueEditorPanel::RenderConditionNode(DialogueNode& node)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 nodeScreen = CanvasToScreen(node.editorPosition);
        float nodeWidth = 160.0f * m_canvasZoom;
        float nodeHeight = 60.0f * m_canvasZoom;
        ImVec2 nodeEnd = ImVec2(nodeScreen.x + nodeWidth, nodeScreen.y + nodeHeight);

        ImVec4 color = GetNodeColor(DialogueNodeType::Condition);
        ImU32 bgCol = (node.id == m_selectedNodeId) ? IM_COL32(55, 55, 65, 240) : IM_COL32(40, 40, 48, 230);

        drawList->AddRectFilled(nodeScreen, nodeEnd, bgCol, 6.0f * m_canvasZoom);
        drawList->AddRect(nodeScreen, nodeEnd, ImGui::ColorConvertFloat4ToU32(color), 6.0f * m_canvasZoom, 0, 2.0f);

        float headerH = 20.0f * m_canvasZoom;
        drawList->AddRectFilled(
            nodeScreen, ImVec2(nodeEnd.x, nodeScreen.y + headerH),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, 0.9f)),
            6.0f * m_canvasZoom, ImDrawFlags_RoundCornersTop);

        float fontSize = 12.0f * m_canvasZoom;
        if (fontSize > 6.0f)
        {
            drawList->AddText(ImVec2(nodeScreen.x + 6.0f * m_canvasZoom, nodeScreen.y + 3.0f * m_canvasZoom),
                              IM_COL32(220, 220, 220, 255), ("Condition: " + node.title).c_str());

            std::string condStr = node.condition.variableName + " == " + node.condition.compareValue;
            drawList->AddText(ImVec2(nodeScreen.x + 6.0f * m_canvasZoom, nodeScreen.y + headerH + 4.0f * m_canvasZoom),
                              IM_COL32(180, 180, 220, 255), condStr.c_str());
        }

        // True output (green)
        float trueY = nodeScreen.y + nodeHeight * 0.35f;
        drawList->AddCircleFilled(ImVec2(nodeEnd.x, trueY), 5.0f * m_canvasZoom, IM_COL32(50, 200, 50, 255));

        // False output (red)
        float falseY = nodeScreen.y + nodeHeight * 0.7f;
        drawList->AddCircleFilled(ImVec2(nodeEnd.x, falseY), 5.0f * m_canvasZoom, IM_COL32(200, 50, 50, 255));

        // Input
        drawList->AddCircleFilled(ImVec2(nodeScreen.x, nodeScreen.y + nodeHeight * 0.5f), 5.0f * m_canvasZoom,
                                  IM_COL32(200, 200, 200, 255));

        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.x >= nodeScreen.x && mousePos.x <= nodeEnd.x && mousePos.y >= nodeScreen.y &&
            mousePos.y <= nodeEnd.y)
        {
            m_hoveredNodeId = node.id;
        }
    }

    void DialogueEditorPanel::RenderEventNode(DialogueNode& node)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 nodeScreen = CanvasToScreen(node.editorPosition);
        float nodeWidth = 160.0f * m_canvasZoom;
        float nodeHeight = 55.0f * m_canvasZoom;
        ImVec2 nodeEnd = ImVec2(nodeScreen.x + nodeWidth, nodeScreen.y + nodeHeight);

        ImVec4 color = GetNodeColor(DialogueNodeType::Event);
        ImU32 bgCol = (node.id == m_selectedNodeId) ? IM_COL32(55, 55, 65, 240) : IM_COL32(40, 40, 48, 230);

        drawList->AddRectFilled(nodeScreen, nodeEnd, bgCol, 6.0f * m_canvasZoom);
        drawList->AddRect(nodeScreen, nodeEnd, ImGui::ColorConvertFloat4ToU32(color), 6.0f * m_canvasZoom, 0, 2.0f);

        float headerH = 20.0f * m_canvasZoom;
        drawList->AddRectFilled(
            nodeScreen, ImVec2(nodeEnd.x, nodeScreen.y + headerH),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, 0.9f)),
            6.0f * m_canvasZoom, ImDrawFlags_RoundCornersTop);

        float fontSize = 12.0f * m_canvasZoom;
        if (fontSize > 6.0f)
        {
            drawList->AddText(ImVec2(nodeScreen.x + 6.0f * m_canvasZoom, nodeScreen.y + 3.0f * m_canvasZoom),
                              IM_COL32(220, 220, 220, 255), ("Event: " + node.title).c_str());

            if (!node.events.empty())
            {
                drawList->AddText(
                    ImVec2(nodeScreen.x + 6.0f * m_canvasZoom, nodeScreen.y + headerH + 4.0f * m_canvasZoom),
                    IM_COL32(220, 180, 100, 255), node.events[0].eventName.c_str());
            }
        }

        // Connectors
        float connY = nodeScreen.y + nodeHeight * 0.5f;
        drawList->AddCircleFilled(ImVec2(nodeEnd.x, connY), 5.0f * m_canvasZoom, IM_COL32(200, 200, 200, 255));
        drawList->AddCircleFilled(ImVec2(nodeScreen.x, connY), 5.0f * m_canvasZoom, IM_COL32(200, 200, 200, 255));

        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.x >= nodeScreen.x && mousePos.x <= nodeEnd.x && mousePos.y >= nodeScreen.y &&
            mousePos.y <= nodeEnd.y)
        {
            m_hoveredNodeId = node.id;
        }
    }

    void DialogueEditorPanel::RenderConnections()
    {
        if (!m_currentDialogue)
            return;

        for (const auto& conn : m_currentDialogue->connections)
        {
            const DialogueNode* fromNode = m_currentDialogue->FindNode(conn.fromNodeId);
            const DialogueNode* toNode = m_currentDialogue->FindNode(conn.toNodeId);
            if (!fromNode || !toNode)
                continue;

            // Calculate output position based on node type and output index
            float fromNodeWidth = 160.0f;
            float fromNodeHeight = 50.0f;
            if (fromNode->type == DialogueNodeType::NPCLine)
            {
                fromNodeWidth = 180.0f;
                fromNodeHeight = 70.0f;
            }
            else if (fromNode->type == DialogueNodeType::PlayerChoice)
            {
                fromNodeWidth = 180.0f;
                fromNodeHeight = 30.0f + static_cast<float>(fromNode->choices.size()) * 18.0f;
            }
            else if (fromNode->type == DialogueNodeType::Condition)
            {
                fromNodeWidth = 160.0f;
                fromNodeHeight = 60.0f;
            }
            else if (fromNode->type == DialogueNodeType::Event)
            {
                fromNodeWidth = 160.0f;
                fromNodeHeight = 55.0f;
            }
            else
            {
                fromNodeWidth = 140.0f;
                fromNodeHeight = 50.0f;
            }

            ImVec2 startCanvas;
            startCanvas.x = fromNode->editorPosition.x + fromNodeWidth;

            if (fromNode->type == DialogueNodeType::Condition)
            {
                startCanvas.y = fromNode->editorPosition.y + fromNodeHeight * (conn.outputIndex == 0 ? 0.35f : 0.7f);
            }
            else if (fromNode->type == DialogueNodeType::PlayerChoice &&
                     conn.outputIndex < static_cast<int>(fromNode->choices.size()))
            {
                float headerH = 20.0f;
                startCanvas.y = fromNode->editorPosition.y + headerH + (6.0f + conn.outputIndex * 18.0f) + 6.0f;
            }
            else
            {
                startCanvas.y = fromNode->editorPosition.y + fromNodeHeight * 0.5f;
            }

            // Input position on target node
            float toNodeHeight = 50.0f;
            if (toNode->type == DialogueNodeType::NPCLine)
                toNodeHeight = 70.0f;
            else if (toNode->type == DialogueNodeType::PlayerChoice)
                toNodeHeight = 30.0f + static_cast<float>(toNode->choices.size()) * 18.0f;
            else if (toNode->type == DialogueNodeType::Condition)
                toNodeHeight = 60.0f;
            else if (toNode->type == DialogueNodeType::Event)
                toNodeHeight = 55.0f;

            ImVec2 endCanvas;
            endCanvas.x = toNode->editorPosition.x;
            endCanvas.y = toNode->editorPosition.y + toNodeHeight * 0.5f;

            ImVec2 startScreen = CanvasToScreen(startCanvas);
            ImVec2 endScreen = CanvasToScreen(endCanvas);

            DrawConnectionCurve(startScreen, endScreen, conn.color, 2.0f);
        }
    }

    void DialogueEditorPanel::RenderNodeInspector()
    {
        if (!m_currentDialogue || m_selectedNodeId == INVALID_DIALOGUE_NODE)
        {
            ImGui::TextDisabled("Select a node to inspect");
            return;
        }

        DialogueNode* node = m_currentDialogue->FindNode(m_selectedNodeId);
        if (!node)
        {
            ImGui::TextDisabled("Node not found");
            return;
        }

        ImGui::Text("ID: %u", node->id);
        ImGui::Text("Type: %s", GetNodeTypeName(node->type));

        char titleBuf[256];
        std::snprintf(titleBuf, sizeof(titleBuf), "%s", node->title.c_str());
        if (ImGui::InputText("Title", titleBuf, sizeof(titleBuf)))
        {
            node->title = titleBuf;
            m_currentDialogue->isModified = true;
        }

        ImGui::Separator();

        switch (node->type)
        {
        case DialogueNodeType::NPCLine:
        {
            // Speaker selector
            if (ImGui::BeginCombo("Speaker", node->speakerName.c_str()))
            {
                for (const auto& character : m_currentDialogue->characters)
                {
                    if (ImGui::Selectable(character.c_str(), character == node->speakerName))
                    {
                        node->speakerName = character;
                        m_currentDialogue->isModified = true;
                    }
                }
                ImGui::EndCombo();
            }

            char textBuf[1024];
            std::snprintf(textBuf, sizeof(textBuf), "%s", node->dialogueText.c_str());
            if (ImGui::InputTextMultiline("Dialogue", textBuf, sizeof(textBuf), ImVec2(-1, 80)))
            {
                node->dialogueText = textBuf;
                m_currentDialogue->isModified = true;
            }

            if (ImGui::DragFloat("Duration", &node->displayDuration, 0.1f, 0.5f, 30.0f, "%.1fs"))
            {
                m_currentDialogue->isModified = true;
            }

            char voiceBuf[256];
            std::snprintf(voiceBuf, sizeof(voiceBuf), "%s", node->voiceClipPath.c_str());
            if (ImGui::InputText("Voice Clip", voiceBuf, sizeof(voiceBuf)))
            {
                node->voiceClipPath = voiceBuf;
                m_currentDialogue->isModified = true;
            }

            char animBuf[256];
            std::snprintf(animBuf, sizeof(animBuf), "%s", node->animationName.c_str());
            if (ImGui::InputText("Animation", animBuf, sizeof(animBuf)))
            {
                node->animationName = animBuf;
                m_currentDialogue->isModified = true;
            }
            break;
        }

        case DialogueNodeType::PlayerChoice:
        {
            ImGui::Text("Choices: %d", static_cast<int>(node->choices.size()));
            if (ImGui::Button(ICON_FA_PLUS " Add Choice"))
            {
                DialogueChoice choice;
                choice.text = "Option " + std::to_string(node->choices.size() + 1);
                node->choices.push_back(choice);
                m_currentDialogue->isModified = true;
            }

            for (int i = 0; i < static_cast<int>(node->choices.size()); ++i)
            {
                ImGui::PushID(i);
                ImGui::Separator();
                ImGui::Text("Choice %d:", i + 1);

                char choiceBuf[512];
                std::snprintf(choiceBuf, sizeof(choiceBuf), "%s", node->choices[i].text.c_str());
                if (ImGui::InputText("Text", choiceBuf, sizeof(choiceBuf)))
                {
                    node->choices[i].text = choiceBuf;
                    m_currentDialogue->isModified = true;
                }

                ImGui::Checkbox("Default", &node->choices[i].isDefault);
                ImGui::DragInt("Priority", &node->choices[i].priority);

                if (ImGui::Button(ICON_FA_TRASH " Remove"))
                {
                    node->choices.erase(node->choices.begin() + i);
                    m_currentDialogue->isModified = true;
                    ImGui::PopID();
                    break;
                }

                ImGui::PopID();
            }
            break;
        }

        case DialogueNodeType::Condition:
        {
            char varBuf[256];
            std::snprintf(varBuf, sizeof(varBuf), "%s", node->condition.variableName.c_str());
            if (ImGui::InputText("Variable", varBuf, sizeof(varBuf)))
            {
                node->condition.variableName = varBuf;
                m_currentDialogue->isModified = true;
            }

            const char* opNames[] = {"Equals", "Not Equals", "Greater Than", "Less Than",
                                     ">=",     "<=",         "Contains",     "Has Flag"};
            int opIdx = static_cast<int>(node->condition.op);
            if (ImGui::Combo("Operator", &opIdx, opNames, 8))
            {
                node->condition.op = static_cast<ConditionOperator>(opIdx);
                m_currentDialogue->isModified = true;
            }

            char valBuf[256];
            std::snprintf(valBuf, sizeof(valBuf), "%s", node->condition.compareValue.c_str());
            if (ImGui::InputText("Value", valBuf, sizeof(valBuf)))
            {
                node->condition.compareValue = valBuf;
                m_currentDialogue->isModified = true;
            }

            if (ImGui::Checkbox("Negate", &node->condition.negate))
            {
                m_currentDialogue->isModified = true;
            }
            break;
        }

        case DialogueNodeType::Event:
        {
            ImGui::Text("Events: %d", static_cast<int>(node->events.size()));
            if (ImGui::Button(ICON_FA_PLUS " Add Event"))
            {
                DialogueEvent evt;
                evt.eventName = "new_event";
                node->events.push_back(evt);
                m_currentDialogue->isModified = true;
            }

            for (int i = 0; i < static_cast<int>(node->events.size()); ++i)
            {
                ImGui::PushID(i);
                ImGui::Separator();

                char evtNameBuf[256];
                std::snprintf(evtNameBuf, sizeof(evtNameBuf), "%s", node->events[i].eventName.c_str());
                if (ImGui::InputText("Event Name", evtNameBuf, sizeof(evtNameBuf)))
                {
                    node->events[i].eventName = evtNameBuf;
                    m_currentDialogue->isModified = true;
                }

                char evtDataBuf[512];
                std::snprintf(evtDataBuf, sizeof(evtDataBuf), "%s", node->events[i].eventData.c_str());
                if (ImGui::InputText("Event Data", evtDataBuf, sizeof(evtDataBuf)))
                {
                    node->events[i].eventData = evtDataBuf;
                    m_currentDialogue->isModified = true;
                }

                if (ImGui::Button(ICON_FA_TRASH " Remove"))
                {
                    node->events.erase(node->events.begin() + i);
                    m_currentDialogue->isModified = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            break;
        }

        case DialogueNodeType::SetVariable:
        {
            char varNameBuf[256];
            std::snprintf(varNameBuf, sizeof(varNameBuf), "%s", node->setVariableName.c_str());
            if (ImGui::InputText("Variable Name", varNameBuf, sizeof(varNameBuf)))
            {
                node->setVariableName = varNameBuf;
                m_currentDialogue->isModified = true;
            }

            char varValBuf[256];
            std::snprintf(varValBuf, sizeof(varValBuf), "%s", node->setVariableValue.c_str());
            if (ImGui::InputText("Value", varValBuf, sizeof(varValBuf)))
            {
                node->setVariableValue = varValBuf;
                m_currentDialogue->isModified = true;
            }
            break;
        }

        case DialogueNodeType::Delay:
        {
            if (ImGui::DragFloat("Delay (s)", &node->delayDuration, 0.1f, 0.0f, 60.0f, "%.1f"))
            {
                m_currentDialogue->isModified = true;
            }
            break;
        }

        default:
            break;
        }

        // Designer notes
        ImGui::Separator();
        char notesBuf[1024];
        std::snprintf(notesBuf, sizeof(notesBuf), "%s", node->designerNotes.c_str());
        if (ImGui::InputTextMultiline("Notes", notesBuf, sizeof(notesBuf), ImVec2(-1, 50)))
        {
            node->designerNotes = notesBuf;
            m_currentDialogue->isModified = true;
        }

        // Delete button
        ImGui::Separator();
        if (node->id != m_currentDialogue->entryNodeId)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button(ICON_FA_TRASH " Delete Node", ImVec2(-1, 0)))
            {
                DeleteNode(node->id);
            }
            ImGui::PopStyleColor();
        }
    }

    void DialogueEditorPanel::RenderCharacterList()
    {
        if (!m_currentDialogue)
            return;

        for (int i = 0; i < static_cast<int>(m_currentDialogue->characters.size()); ++i)
        {
            ImGui::PushID(i);
            char charBuf[256];
            std::snprintf(charBuf, sizeof(charBuf), "%s", m_currentDialogue->characters[i].c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
            if (ImGui::InputText("##char", charBuf, sizeof(charBuf)))
            {
                m_currentDialogue->characters[i] = charBuf;
                m_currentDialogue->isModified = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH))
            {
                m_currentDialogue->characters.erase(m_currentDialogue->characters.begin() + i);
                m_currentDialogue->isModified = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }

        if (ImGui::Button(ICON_FA_PLUS " Add Character", ImVec2(-1, 0)))
        {
            m_currentDialogue->characters.push_back("New Character");
            m_currentDialogue->isModified = true;
        }
    }

    void DialogueEditorPanel::RenderVariableList()
    {
        if (!m_currentDialogue)
            return;

        std::vector<std::string> keysToRemove;

        for (auto& [key, value] : m_currentDialogue->variables)
        {
            ImGui::PushID(key.c_str());
            ImGui::Text("%s", key.c_str());
            ImGui::SameLine();

            char valBuf[256];
            std::snprintf(valBuf, sizeof(valBuf), "%s", value.c_str());
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputText("##val", valBuf, sizeof(valBuf)))
            {
                value = valBuf;
                m_currentDialogue->isModified = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH))
            {
                keysToRemove.push_back(key);
            }
            ImGui::PopID();
        }

        for (const auto& key : keysToRemove)
        {
            m_currentDialogue->variables.erase(key);
            m_currentDialogue->isModified = true;
        }

        ImGui::Separator();
        static char newVarName[128] = "";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60.0f);
        ImGui::InputText("##newvar", newVarName, sizeof(newVarName));
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " Add") && newVarName[0] != '\0')
        {
            m_currentDialogue->variables[newVarName] = "";
            newVarName[0] = '\0';
            m_currentDialogue->isModified = true;
        }
    }

    void DialogueEditorPanel::RenderContextMenu()
    {
        ImGui::OpenPopup("CanvasContextMenu");
        if (ImGui::BeginPopup("CanvasContextMenu"))
        {
            ImGui::Text("Add Node:");
            ImGui::Separator();

            ImVec2 spawnPos = ScreenToCanvas(m_contextMenuPos);

            if (ImGui::MenuItem(ICON_FA_COMMENT " NPC Line"))
            {
                AddNode(DialogueNodeType::NPCLine, spawnPos);
                m_showContextMenu = false;
            }
            if (ImGui::MenuItem(ICON_FA_USER " Player Choice"))
            {
                AddNode(DialogueNodeType::PlayerChoice, spawnPos);
                m_showContextMenu = false;
            }
            if (ImGui::MenuItem(ICON_FA_COG " Condition"))
            {
                AddNode(DialogueNodeType::Condition, spawnPos);
                m_showContextMenu = false;
            }
            if (ImGui::MenuItem(ICON_FA_BOLT " Event"))
            {
                AddNode(DialogueNodeType::Event, spawnPos);
                m_showContextMenu = false;
            }
            if (ImGui::MenuItem(ICON_FA_EDIT " Set Variable"))
            {
                AddNode(DialogueNodeType::SetVariable, spawnPos);
                m_showContextMenu = false;
            }
            if (ImGui::MenuItem(ICON_FA_SYNC_ALT " Random"))
            {
                AddNode(DialogueNodeType::Random, spawnPos);
                m_showContextMenu = false;
            }
            if (ImGui::MenuItem(ICON_FA_CLOCK " Delay"))
            {
                AddNode(DialogueNodeType::Delay, spawnPos);
                m_showContextMenu = false;
            }
            if (ImGui::MenuItem(ICON_FA_TIMES " Exit"))
            {
                AddNode(DialogueNodeType::Exit, spawnPos);
                m_showContextMenu = false;
            }

            if (!ImGui::IsPopupOpen("CanvasContextMenu"))
            {
                m_showContextMenu = false;
            }

            ImGui::EndPopup();
        }
        else
        {
            m_showContextMenu = false;
        }
    }

    void DialogueEditorPanel::RenderPreviewMode()
    {
        if (!m_currentDialogue)
            return;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.15f, 1.0f));
        ImVec2 availSize = ImGui::GetContentRegionAvail();

        ImGui::BeginChild("PreviewContent", availSize, false);

        const DialogueNode* currentNode = m_currentDialogue->FindNode(m_previewCurrentNode);

        if (!currentNode)
        {
            ImGui::SetCursorPosY(availSize.y * 0.4f);
            ImGui::SetCursorPosX(availSize.x * 0.3f);
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Conversation ended.");
            if (ImGui::Button("Restart"))
            {
                m_previewCurrentNode = m_currentDialogue->entryNodeId;
            }
        }
        else
        {
            switch (currentNode->type)
            {
            case DialogueNodeType::Entry:
                // Auto-advance
                m_previewCurrentNode = currentNode->nextNodeId;
                break;

            case DialogueNodeType::NPCLine:
            {
                ImGui::SetCursorPosY(availSize.y * 0.3f);
                ImGui::SetCursorPosX(20.0f);
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[%s]", currentNode->speakerName.c_str());
                ImGui::SetCursorPosX(20.0f);
                ImGui::TextWrapped("%s", currentNode->dialogueText.c_str());
                ImGui::Spacing();
                ImGui::SetCursorPosX(20.0f);
                if (ImGui::Button("Continue >>"))
                {
                    m_previewCurrentNode = currentNode->nextNodeId;
                }
                break;
            }

            case DialogueNodeType::PlayerChoice:
            {
                ImGui::SetCursorPosY(availSize.y * 0.2f);
                ImGui::SetCursorPosX(20.0f);
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "Choose your response:");
                ImGui::Spacing();

                for (int i = 0; i < static_cast<int>(currentNode->choices.size()); ++i)
                {
                    ImGui::SetCursorPosX(40.0f);
                    std::string label = std::to_string(i + 1) + ". " + currentNode->choices[i].text;
                    if (ImGui::Button(label.c_str(), ImVec2(availSize.x - 80.0f, 0)))
                    {
                        m_previewCurrentNode = currentNode->choices[i].targetNodeId;
                    }
                    ImGui::Spacing();
                }
                break;
            }

            case DialogueNodeType::Condition:
                // In preview, always take true path
                m_previewCurrentNode = currentNode->trueTargetId;
                break;

            case DialogueNodeType::Event:
            {
                ImGui::SetCursorPosY(availSize.y * 0.4f);
                ImGui::SetCursorPosX(20.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "[Event: %s]",
                                   currentNode->events.empty() ? "none" : currentNode->events[0].eventName.c_str());
                m_previewCurrentNode = currentNode->nextNodeId;
                break;
            }

            case DialogueNodeType::SetVariable:
            {
                ImGui::SetCursorPosY(availSize.y * 0.4f);
                ImGui::SetCursorPosX(20.0f);
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "[Set %s = %s]",
                                   currentNode->setVariableName.c_str(), currentNode->setVariableValue.c_str());
                m_previewCurrentNode = currentNode->nextNodeId;
                break;
            }

            case DialogueNodeType::Delay:
            {
                ImGui::SetCursorPosY(availSize.y * 0.4f);
                ImGui::SetCursorPosX(20.0f);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[Delay: %.1fs]", currentNode->delayDuration);
                if (ImGui::Button("Skip"))
                {
                    m_previewCurrentNode = currentNode->nextNodeId;
                }
                break;
            }

            case DialogueNodeType::Exit:
                ImGui::SetCursorPosY(availSize.y * 0.4f);
                ImGui::SetCursorPosX(availSize.x * 0.3f);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Conversation ended.");
                if (ImGui::Button("Restart"))
                {
                    m_previewCurrentNode = m_currentDialogue->entryNodeId;
                }
                break;

            case DialogueNodeType::Random:
                if (!currentNode->randomTargets.empty())
                {
                    // Pick first in preview
                    m_previewCurrentNode = currentNode->randomTargets[0];
                }
                else
                {
                    m_previewCurrentNode = currentNode->nextNodeId;
                }
                break;
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void DialogueEditorPanel::RenderMinimap()
    {
        if (!m_currentDialogue || m_currentDialogue->nodes.empty())
            return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 windowSize = ImGui::GetWindowSize();

        float mmWidth = 150.0f;
        float mmHeight = 100.0f;
        ImVec2 mmPos =
            ImVec2(windowPos.x + windowSize.x - mmWidth - 15.0f, windowPos.y + windowSize.y - mmHeight - 15.0f);
        ImVec2 mmEnd = ImVec2(mmPos.x + mmWidth, mmPos.y + mmHeight);

        // Background
        drawList->AddRectFilled(mmPos, mmEnd, IM_COL32(20, 20, 25, 200), 4.0f);
        drawList->AddRect(mmPos, mmEnd, IM_COL32(80, 80, 90, 200), 4.0f);

        // Compute bounds of all nodes
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (const auto& node : m_currentDialogue->nodes)
        {
            minX = std::min(minX, node.editorPosition.x);
            minY = std::min(minY, node.editorPosition.y);
            maxX = std::max(maxX, node.editorPosition.x + 180.0f);
            maxY = std::max(maxY, node.editorPosition.y + 70.0f);
        }

        float rangeX = maxX - minX;
        float rangeY = maxY - minY;
        if (rangeX < 1.0f)
            rangeX = 1.0f;
        if (rangeY < 1.0f)
            rangeY = 1.0f;

        float scaleX = (mmWidth - 10.0f) / rangeX;
        float scaleY = (mmHeight - 10.0f) / rangeY;
        float scale = std::min(scaleX, scaleY);

        for (const auto& node : m_currentDialogue->nodes)
        {
            float nx = mmPos.x + 5.0f + (node.editorPosition.x - minX) * scale;
            float ny = mmPos.y + 5.0f + (node.editorPosition.y - minY) * scale;
            ImVec4 col = GetNodeColor(node.type);
            drawList->AddRectFilled(ImVec2(nx, ny), ImVec2(nx + 6.0f, ny + 4.0f), ImGui::ColorConvertFloat4ToU32(col),
                                    1.0f);
        }
    }

    // =========================================================================
    // Canvas helpers
    // =========================================================================

    ImVec2 DialogueEditorPanel::ScreenToCanvas(ImVec2 screenPos) const
    {
        ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
        return ImVec2((screenPos.x - canvasOrigin.x) / m_canvasZoom - m_canvasOffset.x,
                      (screenPos.y - canvasOrigin.y) / m_canvasZoom - m_canvasOffset.y);
    }

    ImVec2 DialogueEditorPanel::CanvasToScreen(ImVec2 canvasPos) const
    {
        ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
        return ImVec2(canvasOrigin.x + (canvasPos.x + m_canvasOffset.x) * m_canvasZoom,
                      canvasOrigin.y + (canvasPos.y + m_canvasOffset.y) * m_canvasZoom);
    }

    void DialogueEditorPanel::HandleCanvasInput()
    {
        if (!ImGui::IsWindowHovered())
            return;

        ImGuiIO& io = ImGui::GetIO();

        // Zoom with scroll wheel
        if (std::fabs(io.MouseWheel) > 0.0f)
        {
            float zoomDelta = io.MouseWheel * 0.1f;
            m_canvasZoom = std::clamp(m_canvasZoom + zoomDelta, 0.2f, 3.0f);
        }

        // Right-click context menu
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && m_hoveredNodeId == INVALID_DIALOGUE_NODE)
        {
            m_showContextMenu = true;
            m_contextMenuPos = io.MousePos;
        }

        // Left-click select / drag node
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (m_hoveredNodeId != INVALID_DIALOGUE_NODE)
            {
                m_selectedNodeId = m_hoveredNodeId;
                m_isDraggingNode = true;
                DialogueNode* node = m_currentDialogue ? m_currentDialogue->FindNode(m_hoveredNodeId) : nullptr;
                if (node)
                {
                    ImVec2 nodeScreen = CanvasToScreen(node->editorPosition);
                    m_nodeDragOffset = ImVec2(io.MousePos.x - nodeScreen.x, io.MousePos.y - nodeScreen.y);
                }
            }
            else
            {
                m_selectedNodeId = INVALID_DIALOGUE_NODE;
            }
        }

        // Drag node
        if (m_isDraggingNode && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            DialogueNode* node = m_currentDialogue ? m_currentDialogue->FindNode(m_selectedNodeId) : nullptr;
            if (node)
            {
                ImVec2 newScreen = ImVec2(io.MousePos.x - m_nodeDragOffset.x, io.MousePos.y - m_nodeDragOffset.y);
                ImVec2 newCanvas = ScreenToCanvas(newScreen);

                if (m_snapToGrid)
                {
                    newCanvas.x = std::round(newCanvas.x / m_gridSize) * m_gridSize;
                    newCanvas.y = std::round(newCanvas.y / m_gridSize) * m_gridSize;
                }

                node->editorPosition = newCanvas;
                m_currentDialogue->isModified = true;
            }
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            m_isDraggingNode = false;
            m_isDrawingConnection = false;
        }

        // Middle-button pan
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            m_canvasOffset.x += io.MouseDelta.x / m_canvasZoom;
            m_canvasOffset.y += io.MouseDelta.y / m_canvasZoom;
        }

        // Delete key
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_selectedNodeId != INVALID_DIALOGUE_NODE)
        {
            DeleteNode(m_selectedNodeId);
        }
    }

    void DialogueEditorPanel::DrawConnectionCurve(ImVec2 start, ImVec2 end, ImVec4 color, float thickness)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        float dx = end.x - start.x;
        float tangentLen = std::max(50.0f * m_canvasZoom, std::fabs(dx) * 0.4f);

        ImVec2 cp1 = ImVec2(start.x + tangentLen, start.y);
        ImVec2 cp2 = ImVec2(end.x - tangentLen, end.y);

        drawList->AddBezierCubic(start, cp1, cp2, end, ImGui::ColorConvertFloat4ToU32(color), thickness * m_canvasZoom);

        // Arrowhead
        ImVec2 dir = ImVec2(end.x - cp2.x, end.y - cp2.y);
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len > 0.01f)
        {
            dir.x /= len;
            dir.y /= len;
            float arrowSize = 8.0f * m_canvasZoom;
            ImVec2 perp = ImVec2(-dir.y, dir.x);
            ImVec2 p1 = ImVec2(end.x - dir.x * arrowSize + perp.x * arrowSize * 0.4f,
                               end.y - dir.y * arrowSize + perp.y * arrowSize * 0.4f);
            ImVec2 p2 = ImVec2(end.x - dir.x * arrowSize - perp.x * arrowSize * 0.4f,
                               end.y - dir.y * arrowSize - perp.y * arrowSize * 0.4f);
            drawList->AddTriangleFilled(end, p1, p2, ImGui::ColorConvertFloat4ToU32(color));
        }
    }

    ImVec4 DialogueEditorPanel::GetNodeColor(DialogueNodeType type) const
    {
        switch (type)
        {
        case DialogueNodeType::Entry:
            return ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        case DialogueNodeType::NPCLine:
            return ImVec4(0.3f, 0.6f, 0.9f, 1.0f);
        case DialogueNodeType::PlayerChoice:
            return ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
        case DialogueNodeType::Condition:
            return ImVec4(0.7f, 0.3f, 0.9f, 1.0f);
        case DialogueNodeType::Event:
            return ImVec4(0.9f, 0.4f, 0.2f, 1.0f);
        case DialogueNodeType::SetVariable:
            return ImVec4(0.3f, 0.8f, 0.6f, 1.0f);
        case DialogueNodeType::Random:
            return ImVec4(0.8f, 0.8f, 0.2f, 1.0f);
        case DialogueNodeType::Delay:
            return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        case DialogueNodeType::Exit:
            return ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
        default:
            return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        }
    }

    const char* DialogueEditorPanel::GetNodeTypeName(DialogueNodeType type) const
    {
        switch (type)
        {
        case DialogueNodeType::Entry:
            return "Entry";
        case DialogueNodeType::NPCLine:
            return "NPC Line";
        case DialogueNodeType::PlayerChoice:
            return "Player Choice";
        case DialogueNodeType::Condition:
            return "Condition";
        case DialogueNodeType::Event:
            return "Event";
        case DialogueNodeType::SetVariable:
            return "Set Variable";
        case DialogueNodeType::Random:
            return "Random";
        case DialogueNodeType::Delay:
            return "Delay";
        case DialogueNodeType::Exit:
            return "Exit";
        default:
            return "Unknown";
        }
    }

} // namespace SparkEditor
