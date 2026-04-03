/**
 * @file PrototypingPanel.cpp
 * @brief Editor panel for rapid prototyping tools
 */

#include "PrototypingPanel.h"

#include <imgui.h>
#include <string>

namespace SparkEditor
{

    static const char* kShapeNames[] = {"Cube", "Cylinder", "Sphere",  "Ramp", "Stairs",
                                        "Arch", "L-Shape",  "T-Shape", "Ring", "Pipe"};

    PrototypingPanel::PrototypingPanel() : EditorPanel("Prototyping", "Prototyping") {}

    bool PrototypingPanel::Initialize()
    {
        m_system = std::make_unique<PrototypingSystem>();
        m_system->Initialize();
        m_isInitialized = true;
        return true;
    }

    void PrototypingPanel::Update(float deltaTime)
    {
        if (m_system)
            m_system->Update(deltaTime);
    }

    void PrototypingPanel::Render()
    {
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderBlockoutSection();
        RenderTemplateSection();
        RenderRulesSection();
        RenderPlayTestSection();

        EndPanel();
    }

    void PrototypingPanel::Shutdown()
    {
        if (m_system)
            m_system->Shutdown();
    }

    void PrototypingPanel::RenderBlockoutSection()
    {
        if (!ImGui::CollapsingHeader("Blockout Shapes", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        // Shape selector
        int shapeIdx = static_cast<int>(m_selectedShape);
        if (ImGui::Combo("Shape", &shapeIdx, kShapeNames, static_cast<int>(BlockoutShape::Count)))
            m_selectedShape = static_cast<BlockoutShape>(shapeIdx);

        static float placePos[3] = {0.0f, 0.0f, 0.0f};
        ImGui::DragFloat3("Position", placePos, 0.5f);

        if (ImGui::Button("Place Blockout"))
        {
            m_system->PlaceBlockout(m_selectedShape, placePos[0], placePos[1], placePos[2]);
            SetModified(true);
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear All"))
        {
            m_system->ClearAllBlockouts();
            SetModified(true);
        }

        // List placed primitives
        const auto& prims = m_system->GetBlockouts();
        if (!prims.empty())
        {
            ImGui::Separator();
            ImGui::Text("Placed: %zu", prims.size());
            for (const auto& p : prims)
            {
                ImGui::PushID(static_cast<int>(p.primitiveId));
                ImGui::BulletText("%s (%.0f, %.0f, %.0f)", p.name.c_str(), p.posX, p.posY, p.posZ);
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    m_system->RemoveBlockout(p.primitiveId);
                    SetModified(true);
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }
    }

    void PrototypingPanel::RenderTemplateSection()
    {
        if (!ImGui::CollapsingHeader("Game Templates"))
            return;

        const auto& templates = m_system->GetTemplates();
        for (size_t i = 0; i < templates.size(); i++)
        {
            const auto& t = templates[i];
            bool selected = (m_selectedTemplateIdx == static_cast<int>(i));
            if (ImGui::Selectable(t.name.c_str(), selected))
                m_selectedTemplateIdx = static_cast<int>(i);

            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("%s", t.description.c_str());
                ImGui::Text("Camera:%s Player:%s Physics:%s AI:%s UI:%s Net:%s", t.includesCamera ? "Y" : "N",
                            t.includesPlayer ? "Y" : "N", t.includesPhysics ? "Y" : "N", t.includesAI ? "Y" : "N",
                            t.includesUI ? "Y" : "N", t.includesNetworking ? "Y" : "N");
                ImGui::EndTooltip();
            }
        }

        if (ImGui::Button("Apply Template") && m_selectedTemplateIdx >= 0 &&
            m_selectedTemplateIdx < static_cast<int>(templates.size()))
        {
            m_system->ApplyTemplate(templates[m_selectedTemplateIdx].type);
        }
    }

    void PrototypingPanel::RenderRulesSection()
    {
        if (!ImGui::CollapsingHeader("Gameplay Rules"))
            return;

        static char ruleName[64] = "";
        static int triggerIdx = 0;
        static int actionIdx = 0;
        static char actionParam[128] = "";

        static const char* triggers[] = {"OnCollision", "OnTimer", "OnInput", "OnOverlap"};
        static const char* actions[] = {"SpawnEntity", "PlaySound", "AddScore", "SetVariable"};

        ImGui::InputText("Name", ruleName, sizeof(ruleName));
        ImGui::Combo("Trigger", &triggerIdx, triggers, 4);
        ImGui::Combo("Action", &actionIdx, actions, 4);
        ImGui::InputText("Param", actionParam, sizeof(actionParam));

        if (ImGui::Button("Add Rule") && ruleName[0] != '\0')
        {
            m_system->AddRule(ruleName, triggers[triggerIdx], actions[actionIdx], actionParam);
            ruleName[0] = '\0';
            actionParam[0] = '\0';
            SetModified(true);
        }

        const auto& rules = m_system->GetRules();
        for (const auto& r : rules)
        {
            ImGui::PushID(static_cast<int>(r.ruleId));
            bool enabled = r.isEnabled;
            if (ImGui::Checkbox("##en", &enabled))
                m_system->ToggleRule(r.ruleId);
            ImGui::SameLine();
            ImGui::Text("%s: %s -> %s(%s)", r.name.c_str(), r.triggerEvent.c_str(), r.actionType.c_str(),
                        r.actionParam.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
            {
                m_system->RemoveRule(r.ruleId);
                SetModified(true);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    void PrototypingPanel::RenderPlayTestSection()
    {
        if (!ImGui::CollapsingHeader("Play Test"))
            return;

        if (m_system->IsPlaying())
        {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "PLAYING");
            ImGui::Text("Time: %.1fs", m_system->GetPlayTime());
            if (ImGui::Button("Stop"))
                m_system->StopPlayTest();
        }
        else
        {
            if (ImGui::Button("Start Play Test"))
                m_system->StartPlayTest();
        }

        ImGui::Text("Blockouts: %zu | Rules: %zu", m_system->GetBlockouts().size(), m_system->GetRules().size());
    }

} // namespace SparkEditor
