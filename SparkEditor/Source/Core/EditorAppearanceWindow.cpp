/**
 * @file EditorAppearanceWindow.cpp
 * @brief Implementation of the View > Appearance window.
 */

#include "EditorAppearanceWindow.h"

#include "EditorTheme.h"

#include <SparkUI/Runtime.h>
#include <SparkUI/TokenAudit.h>
#include <SparkUI/Widgets.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace SparkEditor
{

    namespace
    {
        namespace Ui = Spark::Ui;

        bool s_openRequested = false;

        /// Stored appearance strings, in the order the radio buttons show them.
        constexpr std::array<const char*, 4> kAppearanceValues = {"dark", "light", "hc", "system"};
#ifdef _WIN32
        constexpr std::array<const char*, 4> kAppearanceLabels = {"Dark", "Light", "High Contrast", "Match Windows"};
#else
        constexpr std::array<const char*, 4> kAppearanceLabels = {"Dark", "Light", "High Contrast", "Match system"};
#endif

        /// Number of fixed scale steps from kMinEditorUiScale to kMaxEditorUiScale inclusive.
        constexpr float kScaleStepSpan = (kMaxEditorUiScale - kMinEditorUiScale) / kEditorUiScaleStep;
        constexpr int kScaleStepCount = static_cast<int>(kScaleStepSpan) + 1;
        // The range must be a whole number of steps, so the count needs no rounding.
        static_assert(static_cast<float>(kScaleStepCount - 1) == kScaleStepSpan,
                      "UI scale range must be an exact multiple of kEditorUiScaleStep");

        /// Small colour sample drawn beside a palette name.
        void Swatch(const ThemeColor& colour)
        {
            const float side = ImGui::GetTextLineHeight();
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + side, min.y + side);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const float rounding = Ui::Px(3.0f);
            drawList->AddRectFilled(min, max, ImGui::GetColorU32(colour.WithAlpha(1.0f).ToImVec4()), rounding);
            drawList->AddRect(min, max, Ui::Col(Ui::Role::BorderSubtle), rounding);
            ImGui::Dummy(ImVec2(side, side));
        }
    } // namespace

    void EditorAppearanceWindow::RequestOpen()
    {
        s_openRequested = true;
    }

    std::optional<Spark::Ui::Appearance> EditorAppearanceWindow::AppearanceOverrideFor(std::string_view appearance)
    {
        if (appearance == "light")
        {
            return Spark::Ui::Appearance::Light;
        }
        if (appearance == "hc")
        {
            return Spark::Ui::Appearance::HighContrast;
        }
        if (appearance == "system")
        {
            return SystemAppearance();
        }
        return std::nullopt;
    }

    Spark::Ui::Appearance EditorAppearanceWindow::SystemAppearance()
    {
#ifdef _WIN32
        HIGHCONTRASTW highContrast{};
        highContrast.cbSize = sizeof(highContrast);
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) != FALSE &&
            (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0)
        {
            return Spark::Ui::Appearance::HighContrast;
        }
        DWORD appsUseLightTheme = 0;
        DWORD valueSize = sizeof(appsUseLightTheme);
        const LSTATUS status =
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsUseLightTheme, &valueSize);
        if (status == ERROR_SUCCESS && appsUseLightTheme != 0)
            return Spark::Ui::Appearance::Light;
#endif
        return Spark::Ui::Appearance::Dark;
    }

    float EditorAppearanceWindow::StepUserScale(float current, int direction)
    {
        const float base = current > 0.0f ? current : 1.0f;
        // Snap to the step grid first so a hand-edited value still lands on a listed step.
        const float steps = std::round((base - kMinEditorUiScale) / kEditorUiScaleStep);
        const float delta = direction > 0 ? 1.0f : (direction < 0 ? -1.0f : 0.0f);
        const float next = kMinEditorUiScale + (steps + delta) * kEditorUiScaleStep;
        return std::clamp(next, kMinEditorUiScale, kMaxEditorUiScale);
    }

    void EditorAppearanceWindow::NormalizePalette(EditorPreferences& prefs)
    {
        if (prefs.palette == "Professional Light")
        {
            prefs.palette = "Ember Studio";
            prefs.appearance = "light";
        }
        else if (prefs.palette == "High Contrast")
        {
            prefs.palette = "Ember Studio";
            prefs.appearance = "hc";
        }
    }

    void EditorAppearanceWindow::ApplyPreferences(const EditorPreferences& prefs)
    {
        // Override and density first so the palette is applied once with both in effect.
        EditorTheme::SetAppearanceOverride(AppearanceOverrideFor(prefs.appearance));
        EditorTheme::SetDensity(prefs.density);
        if (prefs.palette != EditorTheme::GetCurrentThemeName())
        {
            EditorTheme::ApplyTheme(prefs.palette);
        }

        // Only the user factor is requested, so a DPI change queued for the same frame survives; requesting
        // the stored value unconditionally also overrides a different user step still pending.
        Ui::RequestUserScale(prefs.uiScale > 0.0f ? prefs.uiScale : 1.0f);

        Ui::SetReduceMotion(prefs.reduceMotion);
        if (ImGui::GetCurrentContext() != nullptr)
        {
            ImGui::GetIO().ConfigNavCursorVisibleAlways = prefs.alwaysShowFocus;
        }
    }

    bool EditorAppearanceWindow::Draw(EditorPreferences& prefs)
    {
        if (s_openRequested)
        {
            s_openRequested = false;
            m_open = true;
            ImGui::SetNextWindowFocus();
        }
        if (!m_open)
        {
            return false;
        }

        bool changed = false;
        ImGui::SetNextWindowSize(ImVec2(Ui::Em(30.0f), Ui::Em(34.0f)), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Appearance###SparkAppearance", &m_open, ImGuiWindowFlags_NoCollapse))
        {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_open = false;
            }

            changed |= DrawPaletteSection(prefs);
            changed |= DrawAppearanceSection(prefs);
            changed |= DrawScaleSection(prefs);
            changed |= DrawAccessibilitySection(prefs);

            ImGui::Separator();
            DrawAuditBadge();
            ImGui::SameLine();
            if (Ui::SecondaryButton("Reset to defaults"))
            {
                // Keep the user's recent scenes and layout; only the look and feel goes back to defaults.
                EditorPreferences defaults;
                defaults.recentScenes = prefs.recentScenes;
                defaults.lastLayout = prefs.lastLayout;
                if (!(defaults == prefs))
                {
                    prefs = std::move(defaults);
                    changed = true;
                }
            }
        }
        ImGui::End();
        return changed;
    }

    bool EditorAppearanceWindow::DrawPaletteSection(EditorPreferences& prefs)
    {
        if (!Ui::SectionHeader("Palette"))
        {
            return false;
        }

        bool changed = false;
        for (const std::string_view family : kPaletteFamilies)
        {
            const std::string name(family);
            ImGui::PushID(name.c_str());
            if (ImGui::RadioButton(name.c_str(), prefs.palette == name) && prefs.palette != name)
            {
                prefs.palette = name;
                changed = true;
            }
            if (const EditorThemeData* theme = EditorTheme::GetTheme(name))
            {
                // Right-align the three swatches: surface, accent, secondary accent.
                const float swatchesWidth =
                    3.0f * ImGui::GetTextLineHeight() + 2.0f * ImGui::GetStyle().ItemInnerSpacing.x;
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     std::max(0.0f, ImGui::GetContentRegionAvail().x - swatchesWidth));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().ItemInnerSpacing);
                Swatch(theme->background);
                ImGui::SameLine();
                Swatch(theme->accent);
                ImGui::SameLine();
                Swatch(theme->accentSecondary);
                ImGui::PopStyleVar();
            }
            ImGui::PopID();
        }

        const bool isFamily = std::find(std::begin(kPaletteFamilies), std::end(kPaletteFamilies), prefs.palette) !=
                              std::end(kPaletteFamilies);
        if (!isFamily)
        {
            Ui::SecondaryText("Custom theme in use: %s", prefs.palette.c_str());
        }
        return changed;
    }

    bool EditorAppearanceWindow::DrawAppearanceSection(EditorPreferences& prefs)
    {
        if (!Ui::SectionHeader("Appearance"))
        {
            return false;
        }

        bool changed = false;
        for (std::size_t i = 0; i < kAppearanceValues.size(); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine();
            }
            if (ImGui::RadioButton(kAppearanceLabels[i], prefs.appearance == kAppearanceValues[i]) &&
                prefs.appearance != kAppearanceValues[i])
            {
                prefs.appearance = kAppearanceValues[i];
                changed = true;
            }
        }

        ImGui::AlignTextToFramePadding();
        Ui::SecondaryText("Density");
        ImGui::SameLine();
        static constexpr const char* kDensityLabels[] = {"Standard", "Compact"};
        int density = prefs.density == Spark::Ui::Density::Compact ? 1 : 0;
        if (Ui::SegmentedControl("##density", kDensityLabels, 2, &density))
        {
            prefs.density = density == 1 ? Spark::Ui::Density::Compact : Spark::Ui::Density::Standard;
            changed = true;
        }
        return changed;
    }

    bool EditorAppearanceWindow::DrawScaleSection(EditorPreferences& prefs)
    {
        if (!Ui::SectionHeader("UI scale"))
        {
            return false;
        }

        char preview[48];
        if (prefs.uiScale > 0.0f)
        {
            std::snprintf(preview, sizeof(preview), "%.1f%%", prefs.uiScale * 100.0f);
        }
        else
        {
            std::snprintf(preview, sizeof(preview), "Auto (%.0f%%)", Ui::DpiScale() * 100.0f);
        }

        bool changed = false;
        ImGui::SetNextItemWidth(Ui::Em(12.0f));
        if (ImGui::BeginCombo("##uiScale", preview))
        {
            if (ImGui::Selectable("Auto (display scale)", prefs.uiScale <= 0.0f) && prefs.uiScale > 0.0f)
            {
                prefs.uiScale = 0.0f;
                changed = true;
            }
            for (int step = 0; step < kScaleStepCount; ++step)
            {
                const float value = kMinEditorUiScale + static_cast<float>(step) * kEditorUiScaleStep;
                char label[16];
                std::snprintf(label, sizeof(label), "%.1f%%", value * 100.0f);
                const bool selected = prefs.uiScale > 0.0f && std::abs(prefs.uiScale - value) < 0.001f;
                if (ImGui::Selectable(label, selected) && !selected)
                {
                    prefs.uiScale = value;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        Ui::KeyHint("Ctrl+=  Ctrl+-  Ctrl+0");
        return changed;
    }

    bool EditorAppearanceWindow::DrawAccessibilitySection(EditorPreferences& prefs)
    {
        if (!Ui::SectionHeader("Accessibility and workspace"))
        {
            return false;
        }

        bool changed = false;
        changed |= ImGui::Checkbox("Always show keyboard focus", &prefs.alwaysShowFocus);
        ImGui::SameLine();
        Ui::HelpMarker("Draw the focus ring even after mouse input, not only while navigating with the keyboard.");
        changed |= ImGui::Checkbox("Reduce motion", &prefs.reduceMotion);
        ImGui::SameLine();
        Ui::HelpMarker("Turn off fades and animated progress.");
        changed |= ImGui::Checkbox("Show status bar", &prefs.showStatusBar);
        changed |= ImGui::Checkbox("Show Start page at startup", &prefs.showStartPage);
        return changed;
    }

    void EditorAppearanceWindow::DrawAuditBadge()
    {
        const std::uint64_t revision = EditorTheme::GetRevision();
        if (!m_auditValid || revision != m_auditRevision)
        {
            const std::vector<Ui::AuditFailure> failures =
                Ui::AuditTokens(EditorTheme::ToTokens(EditorTheme::GetCurrentThemeData()));
            m_auditFailureCount = failures.size();
            m_auditSummary.clear();
            constexpr std::size_t kListedFailures = 4;
            for (std::size_t i = 0; i < failures.size() && i < kListedFailures; ++i)
            {
                char line[160];
                std::snprintf(line, sizeof(line), "%.*s on %.*s: %.2f:1, needs %.1f:1\n",
                              static_cast<int>(Ui::RoleName(failures[i].fg).size()),
                              Ui::RoleName(failures[i].fg).data(),
                              static_cast<int>(Ui::RoleName(failures[i].bg).size()),
                              Ui::RoleName(failures[i].bg).data(), failures[i].ratio, failures[i].required);
                m_auditSummary += line;
            }
            m_auditRevision = revision;
            m_auditValid = true;
        }

        if (m_auditFailureCount == 0)
        {
            Ui::Badge("Contrast audit passed", Ui::Status::Success);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            {
                ImGui::SetTooltip("Every text and control colour in this theme meets its contrast minimum.");
            }
            return;
        }

        char label[48];
        std::snprintf(label, sizeof(label), "%zu contrast issue%s", m_auditFailureCount,
                      m_auditFailureCount == 1 ? "" : "s");
        Ui::Badge(label, Ui::Status::Warning);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        {
            ImGui::SetTooltip("%s", m_auditSummary.c_str());
        }
    }

} // namespace SparkEditor
