/**
 * @file CommandPalette.cpp
 * @brief Implementation of the command palette overlay
 * @author Spark Engine Team
 * @date 2025
 */

#include "CommandPalette.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>

namespace SparkEditor
{

    CommandPalette::CommandPalette() = default;

    void CommandPalette::RegisterAction(const std::string& name, const std::string& category,
                                        std::function<void()> callback, const std::string& shortcut)
    {
        PaletteAction action;
        action.name = name;
        action.category = category;
        action.shortcut = shortcut;
        action.callback = std::move(callback);
        m_allActions.push_back(std::move(action));
    }

    void CommandPalette::ClearActions()
    {
        m_allActions.clear();
        m_filteredIndices.clear();
    }

    void CommandPalette::Open()
    {
        m_isOpen = true;
        m_focusInput = true;
        m_inputBuffer[0] = '\0';
        m_currentFilter.clear();
        m_selectedIndex = 0;
        FilterActions();
    }

    void CommandPalette::Close()
    {
        m_isOpen = false;
        m_inputBuffer[0] = '\0';
        m_currentFilter.clear();
    }

    void CommandPalette::Toggle()
    {
        if (m_isOpen)
        {
            Close();
        }
        else
        {
            Open();
        }
    }

    void CommandPalette::Render()
    {
        if (!m_isOpen)
        {
            return;
        }

        // Close on Escape
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            Close();
            return;
        }

        // Semi-transparent backdrop
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        drawList->AddRectFilled(ImVec2(0, 0), displaySize, IM_COL32(0, 0, 0, 128));

        // Centered palette window
        float paletteWidth = std::min(600.0f, displaySize.x * 0.6f);
        float paletteX = (displaySize.x - paletteWidth) * 0.5f;
        float paletteY = displaySize.y * 0.15f;

        ImGui::SetNextWindowPos(ImVec2(paletteX, paletteY));
        ImGui::SetNextWindowSize(ImVec2(paletteWidth, 0));
        ImGui::SetNextWindowFocus();

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;

        if (ImGui::Begin("##CommandPalette", &m_isOpen, flags))
        {
            // Search input
            if (m_focusInput)
            {
                ImGui::SetKeyboardFocusHere();
                m_focusInput = false;
            }

            ImGui::SetNextItemWidth(-1);
            bool inputChanged = ImGui::InputTextWithHint("##PaletteInput", ICON_FA_SEARCH " Type a command...",
                                                         m_inputBuffer, sizeof(m_inputBuffer));

            if (inputChanged)
            {
                m_currentFilter = m_inputBuffer;
                m_selectedIndex = 0;
                FilterActions();
            }

            // Handle keyboard navigation
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !m_filteredIndices.empty())
            {
                m_selectedIndex = std::min(m_selectedIndex + 1, static_cast<int>(m_filteredIndices.size()) - 1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            {
                m_selectedIndex = std::max(m_selectedIndex - 1, 0);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !m_filteredIndices.empty())
            {
                ExecuteSelected();
                ImGui::End();
                return;
            }

            ImGui::Separator();

            // Hint text
            if (m_currentFilter.empty())
            {
                ImGui::TextDisabled("Type '>' for commands, '@' for entities, '/' for files");
            }

            // Results list
            float maxHeight = std::min(400.0f, displaySize.y * 0.5f);
            ImGui::BeginChild("PaletteResults", ImVec2(0, maxHeight), false);

            for (size_t i = 0; i < m_filteredIndices.size() && i < 20; ++i)
            {
                const auto& action = m_allActions[m_filteredIndices[i]];
                bool isSelected = (static_cast<int>(i) == m_selectedIndex);

                ImGui::PushID(static_cast<int>(i));

                if (ImGui::Selectable(("##PaletteItem" + std::to_string(i)).c_str(), isSelected, 0, ImVec2(0, 28)))
                {
                    m_selectedIndex = static_cast<int>(i);
                    ExecuteSelected();
                    ImGui::PopID();
                    break;
                }

                // Render action content on same line
                ImGui::SameLine(8);

                // Category badge
                ImVec4 badgeColor = ImVec4(0.3f, 0.5f, 0.8f, 1.0f);
                if (action.category == "Panel")
                {
                    badgeColor = ImVec4(0.3f, 0.7f, 0.3f, 1.0f);
                }
                else if (action.category == "Layout")
                {
                    badgeColor = ImVec4(0.8f, 0.6f, 0.2f, 1.0f);
                }
                else if (action.category == "Scene")
                {
                    badgeColor = ImVec4(0.7f, 0.3f, 0.7f, 1.0f);
                }

                ImGui::TextColored(badgeColor, "[%s]", action.category.c_str());
                ImGui::SameLine();
                ImGui::Text("%s", action.name.c_str());

                if (!action.shortcut.empty())
                {
                    float shortcutWidth = ImGui::CalcTextSize(action.shortcut.c_str()).x;
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - shortcutWidth + 8);
                    ImGui::TextDisabled("%s", action.shortcut.c_str());
                }

                ImGui::PopID();
            }

            if (m_filteredIndices.empty() && !m_currentFilter.empty())
            {
                ImGui::TextDisabled("No matching commands found");
            }

            ImGui::EndChild();

            // Status bar
            ImGui::Separator();
            ImGui::TextDisabled("%zu actions available", m_filteredIndices.size());
        }
        ImGui::End();
    }

    void CommandPalette::FilterActions()
    {
        m_filteredIndices.clear();

        if (m_currentFilter.empty())
        {
            // Show all actions when no filter
            for (size_t i = 0; i < m_allActions.size(); ++i)
            {
                m_filteredIndices.push_back(i);
            }
            return;
        }

        // Check for category prefix
        std::string query = m_currentFilter;
        std::string categoryFilter;

        if (!query.empty())
        {
            if (query[0] == PREFIX_COMMAND)
            {
                categoryFilter = "Command";
                query = query.substr(1);
            }
            else if (query[0] == PREFIX_ENTITY)
            {
                categoryFilter = "Entity";
                query = query.substr(1);
            }
            else if (query[0] == PREFIX_FILE)
            {
                categoryFilter = "Asset";
                query = query.substr(1);
            }
        }

        // Trim leading space after prefix
        while (!query.empty() && query[0] == ' ')
        {
            query = query.substr(1);
        }

        std::string lowerQuery = query;
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

        // Score and filter
        struct ScoredIndex
        {
            size_t index;
            float score;
        };
        std::vector<ScoredIndex> scored;

        for (size_t i = 0; i < m_allActions.size(); ++i)
        {
            const auto& action = m_allActions[i];

            // Category filter
            if (!categoryFilter.empty() && action.category != categoryFilter)
            {
                continue;
            }

            if (lowerQuery.empty())
            {
                scored.push_back({i, 1.0f});
                continue;
            }

            float score = CalculateMatchScore(action.name, lowerQuery);
            if (score > 0.0f)
            {
                scored.push_back({i, score});
            }
        }

        // Sort by score
        std::sort(scored.begin(), scored.end(),
                  [](const ScoredIndex& a, const ScoredIndex& b) { return a.score > b.score; });

        for (const auto& s : scored)
        {
            m_filteredIndices.push_back(s.index);
        }
    }

    void CommandPalette::ExecuteSelected()
    {
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_filteredIndices.size()))
        {
            const auto& action = m_allActions[m_filteredIndices[static_cast<size_t>(m_selectedIndex)]];
            if (action.callback)
            {
                Close();
                action.callback();
            }
        }
    }

    float CommandPalette::CalculateMatchScore(const std::string& text, const std::string& query) const
    {
        std::string lowerText = text;
        std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);

        // Exact match
        if (lowerText == query)
        {
            return 1.0f;
        }

        // Starts with
        if (lowerText.find(query) == 0)
        {
            return 0.9f;
        }

        // Contains
        if (lowerText.find(query) != std::string::npos)
        {
            return 0.7f;
        }

        // Fuzzy subsequence matching
        size_t qi = 0;
        for (size_t ti = 0; ti < lowerText.size() && qi < query.size(); ++ti)
        {
            if (lowerText[ti] == query[qi])
            {
                ++qi;
            }
        }
        if (qi == query.size())
        {
            return 0.3f + 0.2f * (static_cast<float>(query.size()) / static_cast<float>(lowerText.size()));
        }

        return 0.0f;
    }

} // namespace SparkEditor
