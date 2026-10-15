/**
 * @file EditorStartPage.cpp
 * @brief Implementation of the dockable Start tab.
 */

#include "EditorStartPage.h"

#include "EditorIcons.h"

#include <Spark/Version.h>
#include <SparkUI/Brand.h>
#include <SparkUI/Fonts.h>
#include <SparkUI/Runtime.h>
#include <SparkUI/Widgets.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace SparkEditor
{

    namespace
    {
        namespace Ui = Spark::Ui;

        /// Width below which the recent-scenes and shortcut columns stack.
        constexpr float kTwoColumnMinEm = 44.0f;

        struct ShortcutRow
        {
            const char* keys;
            const char* action;
        };

        constexpr ShortcutRow kShortcuts[] = {
            {"Ctrl+N", "New scene"},
            {"Ctrl+O", "Open scene"},
            {"Ctrl+S", "Save scene"},
            {"Ctrl+P", "Command palette"},
            {"Ctrl+F", "Search"},
            {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
            {"W / E / R", "Move / rotate / scale tool"},
            {"F5", "Open Play Control"},
            {"Ctrl+= / Ctrl+- / Ctrl+0", "UI scale up / down / 100%"},
        };
    } // namespace

    StartPageResult EditorStartPage::Draw(EditorPreferences& prefs, unsigned int centreDockId)
    {
        StartPageResult result;
        if (!m_open)
            return result;

        if (centreDockId != 0)
            ImGui::SetNextWindowDockID(centreDockId, ImGuiCond_FirstUseEver);
        if (m_focusRequested)
        {
            ImGui::SetNextWindowFocus();
            m_focusRequested = false;
        }

        const bool visible = ImGui::Begin(kWindowName, &m_open, ImGuiWindowFlags_NoCollapse);
        if (visible)
        {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_open = false;
            }

            DrawHero();
            result.action = DrawActions();

            const bool twoColumns = ImGui::GetContentRegionAvail().x >= Ui::Em(kTwoColumnMinEm);
            if (twoColumns && ImGui::BeginTable("##startColumns", 2, ImGuiTableFlags_NoSavedSettings))
            {
                ImGui::TableNextColumn();
                if (DrawRecentScenes(prefs, result.recentScene))
                    result.action = StartPageAction::OpenRecentScene;
                ImGui::TableNextColumn();
                DrawShortcutSheet();
                ImGui::EndTable();
            }
            else
            {
                if (DrawRecentScenes(prefs, result.recentScene))
                    result.action = StartPageAction::OpenRecentScene;
                DrawShortcutSheet();
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Checkbox("Show at startup", &prefs.showStartPage))
                result.preferencesChanged = true;
            ImGui::SameLine();
            Ui::SecondaryText("Reopen any time from Help > Welcome.");
        }
        ImGui::End();
        return result;
    }

    void EditorStartPage::DrawHero()
    {
        const float height = Ui::Px(112.0f);
        const float markSize = Ui::Px(64.0f);
        const float inset = Ui::Px(24.0f);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + height);
        const Ui::Tokens& tokens = Ui::ActiveTokens();
        const float rounding = Ui::Px(tokens.metrics.rCard);
        // Paint and text roles are chosen by measured contrast against the banner's own colours.
        const Ui::HeroBannerStyle style = Ui::ChooseHeroBannerStyle(tokens);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, Ui::Col(Ui::Role::SurfaceRaised), rounding);
        if (style.gradient)
        {
            // A faint wash keeps text contrast on the raised surface; the full-strength gradient is the bottom rule.
            Ui::DrawBrandGradient(drawList, min, max, style.washAlpha);
            Ui::DrawBrandGradient(drawList, ImVec2(min.x, max.y - Ui::Px(3.0f)), max);
        }
        else
        {
            // High Contrast: a flat surface with a strong outline, no brand colour behind the text.
            const float border = std::max(1.0f, std::round(Ui::Scale()));
            drawList->AddRect(min, max, Ui::Col(Ui::Role::BorderStrong), rounding, 0, border);
        }
        Ui::DrawLogomark(drawList, ImVec2(min.x + inset + markSize * 0.5f, min.y + height * 0.5f), markSize,
                         style.gradient);

        // Line heights are measured with each font pushed: FontSize() is the logical 100 % size, while the
        // band and gap above are device pixels, so the rendered size (logical x UI scale) is what centres.
        float displayHeight = 0.0f;
        {
            Ui::ScopedFont display(Ui::FontRole::Display);
            displayHeight = ImGui::GetFontSize();
        }
        float monoHeight = 0.0f;
        {
            Ui::ScopedFont mono(Ui::FontRole::MonoSmall);
            monoHeight = ImGui::GetFontSize();
        }
        const float lineGap = Ui::Px(4.0f);
        const float textLeft = min.x + inset * 2.0f + markSize;
        const float textTop = min.y + (height - displayHeight - lineGap - monoHeight) * 0.5f;

        ImGui::SetCursorScreenPos(ImVec2(textLeft, textTop));
        {
            Ui::ScopedFont display(Ui::FontRole::Display);
            Ui::TextRole(style.title, "Spark Engine");
        }
        ImGui::SetCursorScreenPos(ImVec2(textLeft, textTop + displayHeight + lineGap));
        {
            Ui::ScopedFont mono(Ui::FontRole::MonoSmall);
            Ui::TextRole(style.caption, "Editor %d.%d.%d", SPARK_ENGINE_VERSION_MAJOR, SPARK_ENGINE_VERSION_MINOR,
                         SPARK_ENGINE_VERSION_PATCH);
        }

        ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + Ui::Px(12.0f)));
    }

    StartPageAction EditorStartPage::DrawActions()
    {
        StartPageAction action = StartPageAction::None;

        // The buttons flow onto a new row instead of clipping when the centre dock is narrow (high UI scale).
        const float rowRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        const auto placeNext = [rowRight](const char* label)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float width = ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
            ImGui::SameLine();
            if (ImGui::GetCursorScreenPos().x + width > rowRight)
                ImGui::NewLine();
        };

        if (Ui::PrimaryButton(ICON_FA_FOLDER_OPEN "  Open Scene..."))
            action = StartPageAction::OpenScene;
        placeNext(ICON_FA_FILE "  New Scene");
        if (Ui::SecondaryButton(ICON_FA_FILE "  New Scene"))
            action = StartPageAction::NewScene;
        placeNext(ICON_FA_FOLDER_OPEN "  Open Project Folder");
        if (Ui::SecondaryButton(ICON_FA_FOLDER_OPEN "  Open Project Folder"))
            action = StartPageAction::OpenProjectFolder;
        placeNext(ICON_FA_UNDO "  Reset workspace layout");
        if (Ui::SecondaryButton(ICON_FA_UNDO "  Reset workspace layout"))
            action = StartPageAction::ResetLayout;
        ImGui::Spacing();
        return action;
    }

    bool EditorStartPage::DrawRecentScenes(const EditorPreferences& prefs, std::string& chosen)
    {
        Ui::SubHeader("Recent scenes");
        if (prefs.recentScenes.empty())
        {
            Ui::SecondaryText("Scenes you open or save appear here.");
            return false;
        }

        bool picked = false;
        for (const std::string& scene : prefs.recentScenes)
        {
            // Split by hand: the stored path is UTF-8, which a narrow std::filesystem::path may not round-trip.
            const std::size_t slash = scene.find_last_of("/\\");
            const std::string fileName = slash == std::string::npos ? scene : scene.substr(slash + 1);
            const std::string folder = slash == std::string::npos ? std::string() : scene.substr(0, slash);
            ImGui::PushID(scene.c_str());
            // Both parts are shortened with a middle ellipsis to the width actually left in this
            // column, so a long path never ends clipped mid-word; the tooltip carries the full path.
            const std::string nameLabel = Ui::FitTextToWidth(fileName, ImGui::GetContentRegionAvail().x);
            if (ImGui::Selectable(nameLabel.c_str(), false, ImGuiSelectableFlags_AllowOverlap))
            {
                chosen = scene;
                picked = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            {
                ImGui::SetTooltip("%s", scene.c_str());
            }
            if (!folder.empty())
            {
                ImGui::SameLine();
                Ui::ScopedFont mono(Ui::FontRole::MonoSmall);
                Ui::TruncatedText(folder, 0.0f, Ui::Role::TextSecondary, scene.c_str());
            }
            ImGui::PopID();
        }
        return picked;
    }

    void EditorStartPage::DrawShortcutSheet()
    {
        Ui::SubHeader("Keyboard shortcuts");
        if (!ImGui::BeginTable("##shortcuts", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
            return;
        for (const ShortcutRow& row : kShortcuts)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Ui::KeyHint(row.keys);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.action);
        }
        ImGui::EndTable();
    }

} // namespace SparkEditor
