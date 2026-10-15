/**
 * @file Widgets.h
 * @brief Spark UI widget kit: role-coloured text, property grids, data tables, toolbars, buttons,
 *        status feedback, cards, the brand band ink and the token focus ring, all styled from the
 *        active tokens.
 *
 * Thread affinity: UI thread only, between ImGui::NewFrame() and ImGui::Render() on the current
 * context (InstallFocusRing: any time on the UI thread). Ownership: widgets keep no state of their
 * own beyond ImGui's per-window storage and the per-context runtime (Runtime.h); the scoped
 * helpers pop exactly what they push. The focus ring's small per-context state is owned by the
 * context through a Shutdown hook and freed in ImGui::DestroyContext.
 */

#pragma once

#include "SparkUI/Enums.h"
#include "SparkUI/Tokens.h"

#include <cstddef>

#include <imgui.h>

namespace Spark::Ui
{
    /// @brief RAII PushStyleColor of a token role.
    class ScopedRoleColor
    {
      public:
        /// @brief Push role @p r (alpha scaled by @p alphaMul) for ImGui colour slot @p idx.
        ScopedRoleColor(ImGuiCol idx, Role r, float alphaMul = 1.0f);
        /// @brief Pop the colour.
        ~ScopedRoleColor();
        ScopedRoleColor(const ScopedRoleColor&) = delete;
        ScopedRoleColor& operator=(const ScopedRoleColor&) = delete;
    };

    /// @brief RAII BeginDisabled/EndDisabled that can explain why the following items are disabled.
    class ScopedDisabled
    {
      public:
        /// @brief Disable the scope when @p disabled; @p reasonTooltip is shown when the last item
        ///        of the scope is hovered while disabled.
        explicit ScopedDisabled(bool disabled, const char* reasonTooltip = nullptr);
        /// @brief End the disabled block.
        ~ScopedDisabled();
        ScopedDisabled(const ScopedDisabled&) = delete;
        ScopedDisabled& operator=(const ScopedDisabled&) = delete;

      private:
        bool m_disabled;
        const char* m_reason;
    };

    /// @brief RAII accent button colours (AccentFill/Hover/Active with TextOnAccent) for a raw
    ///        ImGui::Button.
    class ScopedPrimaryButtonColors
    {
      public:
        /// @brief Push the four colours.
        ScopedPrimaryButtonColors();
        /// @brief Pop the four colours.
        ~ScopedPrimaryButtonColors();
        ScopedPrimaryButtonColors(const ScopedPrimaryButtonColors&) = delete;
        ScopedPrimaryButtonColors& operator=(const ScopedPrimaryButtonColors&) = delete;
    };

    // --- text --------------------------------------------------------------------------------

    /// @brief Formatted text in role @p r.
    void TextRole(Role r, const char* fmt, ...) IM_FMTARGS(2);
    /// @brief Formatted text in TextSecondary.
    void SecondaryText(const char* fmt, ...) IM_FMTARGS(1);
    /// @brief Formatted text in the monospace face.
    void MonoText(const char* fmt, ...) IM_FMTARGS(1);
    /// @brief Status glyph followed by formatted text in the status colour (never colour alone).
    void StatusText(Status s, const char* fmt, ...) IM_FMTARGS(2);
    /// @brief "key  value" row: key in TextSecondary, value in the monospace face.
    void KeyValue(const char* key, const char* fmt, ...) IM_FMTARGS(2);
    /// @brief "(?)" marker that shows @p text as a wrapped tooltip.
    void HelpMarker(const char* text);
    /// @brief Keyboard shortcut hint (Small face, TextSecondary), e.g. "Ctrl+S".
    void KeyHint(const char* keys);

    // --- structure ---------------------------------------------------------------------------

    /// @brief Collapsible section header in the Strong face with an optional icon.
    /// @return True when the section is open.
    bool SectionHeader(const char* label, const char* icon = nullptr, bool defaultOpen = true);
    /// @brief Labelled separator (SeparatorText) in the Strong face.
    void SubHeader(const char* label);
    /// @brief Begin a two-column label/control grid. Narrower than Em(18) it stacks each label
    ///        above its control instead.
    /// @return True when the grid is open; call EndPropertyGrid() only then.
    bool BeginPropertyGrid(const char* id, float labelFraction = 0.38f, float labelMinEm = 7.0f);
    /// @brief Start a grid row: draw @p label and position the cursor for the full-width control.
    void PropertyLabel(const char* label, const char* tooltip = nullptr);
    /// @brief Close the grid opened by BeginPropertyGrid().
    void EndPropertyGrid();
    /// @brief BeginTable with RowBg | BordersInnerH | Resizable | ScrollY plus @p extra.
    /// @return True when the table is visible; call ImGui::EndTable() only then.
    bool BeginDataTable(const char* id, int columns, ImGuiTableFlags extra = 0);
    /// @brief Right-aligned monospace number in the current table cell.
    void TableNumber(const char* fmt, ...) IM_FMTARGS(1);
    /// @brief Vertical separator line between items on the same row.
    void VerticalSeparator();

    // --- panel toolbar -----------------------------------------------------------------------

    /// @brief Begin a panel toolbar row.
    /// @return True when the toolbar is visible; call EndPanelToolbar() only then.
    bool BeginPanelToolbar(const char* id);
    /// @brief End the toolbar row.
    void EndPanelToolbar();
    /// @brief Search box with a magnifier glyph and hint.
    /// @return True when the text changed.
    bool SearchField(const char* id, char* buf, std::size_t bufSize, const char* hint = "Search");
    /// @brief Search box bound to an ImGuiTextFilter.
    /// @return True when the filter changed.
    bool SearchField(const char* id, ImGuiTextFilter& filter, const char* hint = "Search");
    /// @brief Square icon button (at least Px(24)) with tooltip and optional selected state.
    /// @return True when clicked (never while disabled).
    bool IconButton(const char* id, const char* icon, const char* tooltip, bool selected = false, bool enabled = true);
    /// @brief Row of mutually exclusive buttons.
    /// @return True when @p current changed.
    bool SegmentedControl(const char* id, const char* const* labels, int count, int* current);
    /// @brief Separator between toolbar groups.
    void ToolbarSeparator();

    // --- buttons -----------------------------------------------------------------------------

    /// @brief Accent-filled button for the primary action.
    bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0));
    /// @brief Neutral button.
    bool SecondaryButton(const char* label, ImVec2 size = ImVec2(0, 0));
    /// @brief Error-tinted button for destructive actions: ErrorText on ErrorFill, hovered and
    ///        pressed states tint the fill (DangerTintFill) so the label keeps 4.5:1.
    bool DangerButton(const char* label, ImVec2 size = ImVec2(0, 0));

    // --- feedback ----------------------------------------------------------------------------

    /// @brief Rounded chip with glyph, word-carrying label and status colours.
    void StatusChip(Status s, const char* label, const char* tooltip = nullptr);
    /// @brief Small label badge.
    void Badge(const char* label, Status s = Status::Neutral);
    /// @brief Full-width message banner with glyph and status word.
    /// @return True when it was dismissed this frame.
    bool Banner(Status s, const char* message, bool dismissible = false);
    /// @brief Centered empty-state message with an optional action.
    /// @return True when the action button was clicked.
    bool EmptyState(const char* icon, const char* title, const char* hint = nullptr, const char* actionLabel = nullptr);
    /// @brief Explain that @p feature is unavailable and why.
    void UnavailableFeature(const char* feature, const char* reason);
    /// @brief Indeterminate progress bar; a static stripe when ReduceMotion().
    void ProgressIndeterminate(const char* id, ImVec2 size);
    /// @brief Modal confirmation of a destructive action. Cancel has default focus; Esc cancels.
    /// @return True when the user confirmed this frame.
    bool ConfirmDestructive(const char* popupId, const char* title, const char* body, const char* confirmLabel);
    /// @brief Draw ImGui's keyboard/gamepad focus ring at the token width in every window of @p ctx.
    /// @details Spec section 2.6: the ring is Metrics::focusRing logical pixels wide (2, or 3 in
    ///          High Contrast), offset 1 outside the item. ImGui's own cursor is hard-wired to
    ///          max(2, 1.5 x scale) pixels, so for each frame the hook hides it (ImGuiCol_NavCursor
    ///          reads transparent between NewFrame and EndFrame; read Role::BorderFocus for the
    ///          focus colour instead) and redraws it at the start of EndFrame into the focused
    ///          window's draw list. Idempotent: a second call on the same context does nothing.
    ///          Every Spark UI host calls it once after Install().
    void InstallFocusRing(ImGuiContext* ctx);

    // --- brand -------------------------------------------------------------------------------

    /// @brief How a brand band (launcher rail header, installer step rail) is painted.
    struct BrandBandStyle
    {
        bool gradient = true;          ///< Paint the brand gradient; false = flat SurfaceRaised.
        Role ink = Role::TextOnAccent; ///< Role for the band's text and logomark.
        float worstContrast = 0.0f;    ///< Ink contrast on the band's least favourable colour.
    };

    /// @brief Choose the band paint and ink for @p t by measured contrast.
    /// @details The gradient stops are fixed (Brand.h), so no appearance role is assumed to read on
    ///          them: TextOnAccent and TextPrimary are measured on every stop and the better one
    ///          wins. When neither reaches the appearance's text minimum (High Contrast needs 7:1)
    ///          the band is flat SurfaceRaised with TextPrimary instead.
    BrandBandStyle ChooseBrandBandStyle(const Tokens& t) noexcept;

    // --- cards -------------------------------------------------------------------------------

    /// @brief Begin a selectable card (launcher, installer, start page).
    /// @return True when the card is visible; call EndCard() regardless (it mirrors EndChild).
    bool BeginCard(const char* id, ImVec2 size, bool selected = false);
    /// @brief True when the card currently being built was clicked this frame.
    bool IsCardClicked();
    /// @brief End the card.
    void EndCard();
} // namespace Spark::Ui
