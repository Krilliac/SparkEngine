/**
 * @file DestructionEditorPanel.cpp
 * @brief Implementation of the destruction system editor panel
 */

#include "DestructionEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>

namespace SparkEditor
{

    DestructionEditorPanel::DestructionEditorPanel() : EditorPanel("Destruction Editor", "destruction_editor_panel") {}

    bool DestructionEditorPanel::Initialize()
    {
        std::cout << "Initializing Destruction Editor panel\n";

        // Add a sample fracture pattern
        PatternInfo defaultPattern;
        snprintf(defaultPattern.name, sizeof(defaultPattern.name), "Default_Crate");
        snprintf(defaultPattern.destructionSound, sizeof(defaultPattern.destructionSound), "sfx_crate_break");
        snprintf(defaultPattern.particleEffect, sizeof(defaultPattern.particleEffect), "vfx_debris_wood");

        for (int i = 0; i < 4; ++i)
        {
            FracturePieceInfo piece;
            snprintf(piece.name, sizeof(piece.name), "Piece_%d", i);
            snprintf(piece.meshName, sizeof(piece.meshName), "crate_fragment_%d", i);
            piece.mass = 0.5f + static_cast<float>(i) * 0.2f;
            defaultPattern.pieces.push_back(piece);
        }
        m_patterns.push_back(std::move(defaultPattern));

        return true;
    }

    void DestructionEditorPanel::Update(float /*deltaTime*/) {}

    void DestructionEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("DestructionTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_BOMB " Patterns"))
                {
                    RenderPatternList();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_EDIT " Editor"))
                {
                    RenderPatternEditor();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_FIRE " Debris"))
                {
                    RenderDebrisMonitor();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void DestructionEditorPanel::Shutdown()
    {
        std::cout << "Shutting down Destruction Editor panel\n";
    }

    void DestructionEditorPanel::RenderPatternList()
    {
        if (ImGui::Button(ICON_FA_PLUS " New Pattern"))
        {
            PatternInfo newPattern;
            snprintf(newPattern.name, sizeof(newPattern.name), "Pattern_%d", static_cast<int>(m_patterns.size()));
            m_patterns.push_back(std::move(newPattern));
        }

        ImGui::Separator();

        for (int i = 0; i < static_cast<int>(m_patterns.size()); ++i)
        {
            bool selected = (m_selectedPattern == i);
            if (ImGui::Selectable(m_patterns[i].name, selected))
            {
                m_selectedPattern = i;
                m_selectedPiece = -1;
            }
        }
    }

    void DestructionEditorPanel::RenderPatternEditor()
    {
        if (m_selectedPattern < 0 || m_selectedPattern >= static_cast<int>(m_patterns.size()))
        {
            ImGui::TextDisabled("Select a pattern from the Patterns tab.");
            return;
        }

        auto& pattern = m_patterns[m_selectedPattern];

        ImGui::InputText("Name", pattern.name, sizeof(pattern.name));
        ImGui::InputText("Sound", pattern.destructionSound, sizeof(pattern.destructionSound));
        ImGui::InputText("Particle", pattern.particleEffect, sizeof(pattern.particleEffect));

        ImGui::Separator();
        ImGui::Text("Pieces (%d)", static_cast<int>(pattern.pieces.size()));

        if (ImGui::Button(ICON_FA_PLUS " Add Piece"))
        {
            FracturePieceInfo piece;
            snprintf(piece.name, sizeof(piece.name), "Piece_%d", static_cast<int>(pattern.pieces.size()));
            pattern.pieces.push_back(piece);
        }

        if (ImGui::BeginTable("PieceTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Mesh");
            ImGui::TableSetupColumn("Mass", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Lifetime", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(pattern.pieces.size()); ++i)
            {
                auto& piece = pattern.pieces[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##name", piece.name, sizeof(piece.name));

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##mesh", piece.meshName, sizeof(piece.meshName));

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat("##mass", &piece.mass, 0.1f, 0.01f, 100.0f, "%.2f");

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat("##life", &piece.lifetime, 0.1f, 0.5f, 60.0f, "%.1f");

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void DestructionEditorPanel::RenderDebrisMonitor()
    {
        ImGui::Text("Active Debris: %d / %d", m_activeDebrisCount, m_maxDebris);

        float ratio =
            m_maxDebris > 0 ? static_cast<float>(m_activeDebrisCount) / static_cast<float>(m_maxDebris) : 0.0f;
        ImGui::ProgressBar(ratio, ImVec2(-1, 0));

        ImGui::Separator();
        ImGui::DragInt("Max Debris", &m_maxDebris, 1.0f, 32, 2048);
        ImGui::DragFloat("Lifetime Multiplier", &m_debrisLifetimeMultiplier, 0.1f, 0.1f, 5.0f, "%.1fx");

        ImGui::Separator();
        if (ImGui::Button(ICON_FA_TRASH " Clear All Debris"))
        {
            m_activeDebrisCount = 0;
        }
    }

} // namespace SparkEditor
