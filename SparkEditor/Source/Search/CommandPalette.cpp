/**
 * @file CommandPalette.cpp
 * @brief Implementation of the command palette overlay
 * @author Spark Engine Team
 * @date 2025
 */

#include "CommandPalette.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <SparkUI/Fonts.h>
#include <SparkUI/Runtime.h>
#include <SparkUI/Widgets.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>

namespace SparkEditor
{

    CommandPalette::CommandPalette() = default;

    void CommandPalette::RegisterAction(const std::string& name, const std::string& category,
                                        std::function<void()> callback, const std::string& shortcut)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Editor, name);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Editor, category);
        PaletteAction action;
        action.name = name;
        action.category = category;
        action.shortcut = shortcut;
        action.callback = std::move(callback);
        m_allActions.push_back(std::move(action));
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Registered command: '%s' in category '%s'", name.c_str(),
                        category.c_str());
    }

    void CommandPalette::ClearActions()
    {
        m_allActions.clear();
        m_filteredIndices.clear();
        m_selectedIndex = 0;
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
    // NOTE: Intentionally exceeds the 60-line guideline -- scrim, search field, result rows and footer in order.
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        namespace Ui = Spark::Ui;
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

        // Scrim: dims the editor behind the palette and closes the palette when clicked.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Ui::ColV(Ui::Role::ModalScrim));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        const ImGuiWindowFlags scrimFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##CommandPaletteScrim", nullptr, scrimFlags) && ImGui::IsWindowHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            Close();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        if (!m_isOpen)
        {
            return;
        }

        // Centered palette window
        const float paletteWidth = std::min(Ui::Em(46.0f), viewport->Size.x * 0.6f);
        const float paletteX = viewport->Pos.x + (viewport->Size.x - paletteWidth) * 0.5f;
        const float paletteY = viewport->Pos.y + viewport->Size.y * 0.15f;

        ImGui::SetNextWindowPos(ImVec2(paletteX, paletteY));
        ImGui::SetNextWindowSize(ImVec2(paletteWidth, 0.0f));
        ImGui::SetNextWindowFocus();

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, Ui::ColV(Ui::Role::SurfaceOverlay));
        ImGui::PushStyleColor(ImGuiCol_Border, Ui::ColV(Ui::Role::BorderSubtle));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Ui::Px(Ui::ActiveTokens().metrics.rPopup));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Ui::Px2(10.0f, 10.0f));
        const bool visible = ImGui::Begin("##CommandPalette", &m_isOpen, flags);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        if (visible)
        {
            // Search input
            if (m_focusInput)
            {
                ImGui::SetKeyboardFocusHere();
                m_focusInput = false;
            }
            if (Ui::SearchField("##PaletteInput", m_inputBuffer, sizeof(m_inputBuffer), "Type a command..."))
            {
                SetFilter(m_inputBuffer);
            }

            // Handle keyboard navigation
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                MoveSelection(1);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                MoveSelection(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) && GetVisibleActionCount() > 0)
            {
                if (ExecuteSelected())
                {
                    ImGui::End();
                    return;
                }
            }

            // Hint text
            if (m_currentFilter.empty())
            {
                Ui::SecondaryText("Type '>' for commands, scene actions, and layouts");
            }
            ImGui::Separator();

            // Results list
            const float maxHeight = std::min(Ui::Em(28.0f), viewport->Size.y * 0.5f);
            const float rowHeight = ImGui::GetFrameHeight() + Ui::Px(6.0f);
            ImGui::BeginChild("PaletteResults", ImVec2(0.0f, maxHeight), ImGuiChildFlags_None);

            for (size_t i = 0; i < GetVisibleActionCount(); ++i)
            {
                const auto& action = m_allActions[m_filteredIndices[i]];
                const bool isSelected = (static_cast<int>(i) == m_selectedIndex);

                ImGui::PushID(static_cast<int>(i));
                const float rowLeft = ImGui::GetCursorPosX();
                const float rowTop = ImGui::GetCursorPosY();
                const float rowRight = rowLeft + ImGui::GetContentRegionAvail().x;
                const float textTop = rowTop + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;

                if (ImGui::Selectable("##PaletteItem", isSelected, ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(0.0f, rowHeight)))
                {
                    m_selectedIndex = static_cast<int>(i);
                    ExecuteSelected();
                    ImGui::PopID();
                    break;
                }
                if (isSelected && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
                    ImGui::SetScrollHereY();

                // Category badge, name, then the shortcut right-aligned on the same row.
                ImGui::SameLine(rowLeft + Ui::Px(8.0f));
                ImGui::SetCursorPosY(textTop);
                Ui::Badge(action.category.c_str(), Ui::Status::Neutral);
                ImGui::SameLine();
                ImGui::SetCursorPosY(textTop);
                ImGui::TextUnformatted(action.name.c_str());

                if (!action.shortcut.empty())
                {
                    float shortcutWidth = 0.0f;
                    {
                        Ui::ScopedFont hintFont(Ui::FontRole::Small);
                        shortcutWidth = ImGui::CalcTextSize(action.shortcut.c_str()).x;
                    }
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rowRight - shortcutWidth - Ui::Px(8.0f)));
                    ImGui::SetCursorPosY(textTop);
                    Ui::KeyHint(action.shortcut.c_str());
                }
                ImGui::SetCursorPosY(rowTop + rowHeight + ImGui::GetStyle().ItemSpacing.y);

                ImGui::PopID();
            }

            if (m_filteredIndices.empty() && !m_currentFilter.empty())
            {
                Ui::SecondaryText("No matching commands found");
            }

            ImGui::EndChild();

            // Footer
            ImGui::Separator();
            if (GetVisibleActionCount() < m_filteredIndices.size())
                Ui::SecondaryText("%zu of %zu actions shown", GetVisibleActionCount(), m_filteredIndices.size());
            else
                Ui::SecondaryText("%zu actions available", m_filteredIndices.size());
        }
        ImGui::End();
    }

    void CommandPalette::SetFilter(const std::string& filter)
    {
        m_currentFilter = filter;
        m_selectedIndex = 0;
        FilterActions();
    }

    size_t CommandPalette::GetVisibleActionCount() const
    {
        return std::min(m_filteredIndices.size(), MaxVisibleResults);
    }

    void CommandPalette::MoveSelection(int delta)
    {
        const size_t visibleCount = GetVisibleActionCount();
        if (visibleCount == 0)
        {
            m_selectedIndex = 0;
            return;
        }

        const int lastVisibleIndex = static_cast<int>(visibleCount) - 1;
        m_selectedIndex = std::clamp(m_selectedIndex + delta, 0, lastVisibleIndex);
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

        // Check for category prefix (the filter is known to be non-empty here)
        std::string query = m_currentFilter;
        std::string categoryFilter;

        if (query[0] == PREFIX_COMMAND)
        {
            categoryFilter = ">";
            query = query.substr(1);
        }

        // Trim leading space after prefix
        while (!query.empty() && query[0] == ' ')
        {
            query = query.substr(1);
        }

        std::string lowerQuery = query;
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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
            if (categoryFilter == ">" && !IsCommandLikeCategory(action.category))
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

        const int lastVisibleIndex = static_cast<int>(GetVisibleActionCount()) - 1;
        m_selectedIndex = lastVisibleIndex >= 0 ? std::clamp(m_selectedIndex, 0, lastVisibleIndex) : 0;
    }

    bool CommandPalette::ExecuteSelected()
    {
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(GetVisibleActionCount()))
        {
            size_t actionIdx = m_filteredIndices[static_cast<size_t>(m_selectedIndex)];
            if (actionIdx >= m_allActions.size())
                return false;
            const auto& action = m_allActions[actionIdx];
            if (action.callback)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Executing command: '%s'", action.name.c_str());
                Close();
                action.callback();
                return true;
            }
        }
        return false;
    }

    bool CommandPalette::IsCommandLikeCategory(const std::string& category)
    {
        return category == "Command" || category == "Scene" || category == "Layout";
    }

    float CommandPalette::CalculateMatchScore(const std::string& text, const std::string& query) const
    {
        std::string lowerText = text;
        std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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
        if (lowerText.contains(query))
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
