/**
 * @file PrototypingPanel.cpp
 * @brief Editor panel for rapid prototyping tools
 */

#include "PrototypingPanel.h"

#include "../Core/EditorIcons.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

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
        if (!Spark::Ui::SectionHeader("Blockout Shapes", ICON_FA_CUBES, true))
            return;

        // Shape selector
        int shapeIdx = static_cast<int>(m_selectedShape);
        if (ImGui::Combo("Shape", &shapeIdx, kShapeNames, static_cast<int>(BlockoutShape::Count)))
            m_selectedShape = static_cast<BlockoutShape>(shapeIdx);

        static float placePos[3] = {0.0f, 0.0f, 0.0f};
        ImGui::DragFloat3("Position", placePos, 0.5f);

        if (Spark::Ui::PrimaryButton(ICON_FA_PLUS " Place Blockout"))
        {
            m_system->PlaceBlockout(m_selectedShape, placePos[0], placePos[1], placePos[2]);
            SetModified(true);
        }

        ImGui::SameLine();
        if (Spark::Ui::DangerButton(ICON_FA_TRASH " Clear All"))
        {
            m_system->ClearAllBlockouts();
            SetModified(true);
        }

        // List placed primitives
        const auto& prims = m_system->GetBlockouts();
        if (!prims.empty())
        {
            ImGui::Separator();
            Spark::Ui::KeyValue("Placed:", "%zu", prims.size());
            for (const auto& p : prims)
            {
                ImGui::PushID(static_cast<int>(p.primitiveId));
                ImGui::BulletText("%s (%.0f, %.0f, %.0f)", p.name.c_str(), p.posX, p.posY, p.posZ);
                ImGui::SameLine();
                if (Spark::Ui::IconButton("remove", ICON_FA_XMARK, "Remove blockout"))
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
        if (!Spark::Ui::SectionHeader("Game Templates", ICON_FA_TH_LARGE, false))
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
                ImGui::TextUnformatted(t.description.c_str());
                Spark::Ui::SecondaryText("Camera:%s Player:%s Physics:%s AI:%s UI:%s Net:%s",
                                         t.includesCamera ? "Y" : "N", t.includesPlayer ? "Y" : "N",
                                         t.includesPhysics ? "Y" : "N", t.includesAI ? "Y" : "N",
                                         t.includesUI ? "Y" : "N", t.includesNetworking ? "Y" : "N");
                ImGui::EndTooltip();
            }
        }

        if (Spark::Ui::PrimaryButton("Apply Template") && m_selectedTemplateIdx >= 0 &&
            m_selectedTemplateIdx < static_cast<int>(templates.size()))
        {
            m_system->ApplyTemplate(templates[m_selectedTemplateIdx].type);
        }
    }

    void PrototypingPanel::RenderRulesSection()
    {
        if (!Spark::Ui::SectionHeader("Gameplay Rules", ICON_FA_LIST_ALT, false))
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

        if (Spark::Ui::SecondaryButton(ICON_FA_PLUS " Add Rule") && ruleName[0] != '\0')
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
            if (Spark::Ui::IconButton("remove", ICON_FA_XMARK, "Remove rule"))
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
        if (!Spark::Ui::SectionHeader("Play Test", ICON_FA_PLAY, false))
            return;

        if (m_system->IsPlaying())
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Success, "PLAYING");
            Spark::Ui::KeyValue("Time:", "%.1fs", m_system->GetPlayTime());
            if (Spark::Ui::SecondaryButton(ICON_FA_STOP " Stop"))
                m_system->StopPlayTest();
        }
        else
        {
            if (Spark::Ui::PrimaryButton(ICON_FA_PLAY " Start Play Test"))
                m_system->StartPlayTest();
        }

        Spark::Ui::KeyValue("Blockouts:", "%zu", m_system->GetBlockouts().size());
        ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
        Spark::Ui::KeyValue("Rules:", "%zu", m_system->GetRules().size());
    }

} // namespace SparkEditor
