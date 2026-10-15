/**
 * @file Widgets.cpp
 * @brief Spark UI widget kit implementation.
 */

#include "SparkUI/Widgets.h"

#include "RuntimeState.h"
#include "SparkUI/Brand.h"
#include "SparkUI/Color.h"
#include "SparkUI/Fonts.h"
#include "SparkUI/Glyphs.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Style.h"
#include "SparkUI/TokenAudit.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <memory>

#include <imgui_internal.h>

namespace Spark::Ui
{
    namespace
    {
        constexpr std::size_t kFormatBufferSize = 1024;
        constexpr float kPropertyGridStackEm = 18.0f;

        struct FormatBuffer
        {
            char text[kFormatBufferSize] = {};
        };

        FormatBuffer FormatV(const char* fmt, va_list args)
        {
            FormatBuffer buffer;
            if (fmt != nullptr)
            {
                std::vsnprintf(buffer.text, sizeof(buffer.text), fmt, args);
            }
            return buffer;
        }

        /// "icon  label" (or the label alone) into @p out.
        void JoinIconLabel(char* out, std::size_t size, const char* icon, const char* label)
        {
            if (icon != nullptr && icon[0] != '\0')
            {
                std::snprintf(out, size, "%s  %s", icon, label != nullptr ? label : "");
            }
            else
            {
                std::snprintf(out, size, "%s", label != nullptr ? label : "");
            }
        }

        void ItemTooltip(const char* text)
        {
            if (text != nullptr && text[0] != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
                ImGui::BeginTooltip())
            {
                ImGui::PushTextWrapPos(Em(30.0f));
                ImGui::TextUnformatted(text);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }

        /// Per-context focus ring state: ImGui's nav cursor colour while it is hidden for a frame.
        struct FocusRingState
        {
            ImVec4 hiddenColour{};
            bool hiding = false;
        };

        ImGuiID FocusRingOwnerId()
        {
            return ImHashStr("SparkUi.FocusRing");
        }

        void DestroyFocusRingState(ImGuiContext*, ImGuiContextHook* hook)
        {
            // Adopt the state InstallFocusRing released to the context so it is freed exactly once.
            std::unique_ptr<FocusRingState> owned(static_cast<FocusRingState*>(hook->UserData));
            hook->UserData = nullptr;
        }

        /// NewFramePost: make ImGui's own 2 px cursor transparent for this frame.
        void HideNativeNavCursor(ImGuiContext* ctx, ImGuiContextHook* hook)
        {
            auto* state = static_cast<FocusRingState*>(hook->UserData);
            if (state == nullptr)
            {
                return;
            }
            ImVec4& colour = ctx->Style.Colors[ImGuiCol_NavCursor];
            state->hiding = colour.w > 0.0f;
            if (state->hiding)
            {
                state->hiddenColour = colour;
                colour.w = 0.0f;
            }
        }

        /// EndFramePre: restore the cursor colour and draw the token-width ring around the nav item.
        void DrawTokenFocusRing(ImGuiContext* ctx, ImGuiContextHook* hook)
        {
            auto* state = static_cast<FocusRingState*>(hook->UserData);
            if (state == nullptr || !state->hiding)
            {
                return;
            }
            ImVec4& colour = ctx->Style.Colors[ImGuiCol_NavCursor];
            if (colour.w == 0.0f)
            {
                // Still the transparent value set at NewFrame; a theme applied mid-frame wins.
                colour = state->hiddenColour;
            }
            state->hiding = false;

            ImGuiContext& g = *ctx;
            ImGuiWindow* window = g.NavWindow;
            if (!g.NavCursorVisible || g.NavId == 0 || !g.NavIdIsAlive || window == nullptr || !window->Active ||
                window->Hidden || window->SkipRefresh || window->DC.NavHideHighlightOneFrame ||
                (g.NavIdItemFlags & ImGuiItemFlags_NoNav) != 0)
            {
                return;
            }
            const ImGuiNavLayer layer = g.NavLayer;
            ImRect item = ImGui::WindowRectRelToAbs(window, window->NavRectRel[layer]);
            item.ClipWith(layer == ImGuiNavLayer_Main ? window->InnerClipRect : window->Rect());
            if (item.GetWidth() <= 0.0f || item.GetHeight() <= 0.0f)
            {
                return;
            }
            const float thickness = std::max(1.0f, Px(ActiveTokens().metrics.focusRing));
            const float offset = Px(1.0f) + thickness * 0.5f;
            const ImVec2 min(item.Min.x - offset, item.Min.y - offset);
            const ImVec2 max(item.Max.x + offset, item.Max.y + offset);
            // Like ImGui's own cursor, the ring may extend past the window clip rectangle.
            ImDrawList* drawList = window->DrawList;
            drawList->PushClipRect(ImVec2(min.x - thickness, min.y - thickness),
                                   ImVec2(max.x + thickness, max.y + thickness), false);
            drawList->AddRect(min, max, ImGui::GetColorU32(colour), Px(ActiveTokens().metrics.rFrame) + offset, 0,
                              thickness);
            drawList->PopClipRect();
        }

        /// Rounded pill at the cursor, reserved as one item.
        void Pill(const char* text, Role fill, Role ink, Role border, float rounding)
        {
            const ImVec2 padding(Px(6.0f), Px(2.0f));
            const ImVec2 textSize = ImGui::CalcTextSize(text);
            const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + size.x, min.y + size.y);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const float radius = rounding < 0.0f ? size.y * 0.5f : rounding;
            drawList->AddRectFilled(min, max, Col(fill), radius);
            drawList->AddRect(min, max, Col(border, 0.6f), radius, 0, std::max(1.0f, Px(1.0f)));
            drawList->AddText(ImVec2(min.x + padding.x, min.y + padding.y), Col(ink), text);
            ImGui::Dummy(size);
        }
    } // namespace

    // --- scoped helpers ----------------------------------------------------------------------

    ScopedRoleColor::ScopedRoleColor(ImGuiCol idx, Role r, float alphaMul)
    {
        ImGui::PushStyleColor(idx, ColV(r, alphaMul));
    }

    ScopedRoleColor::~ScopedRoleColor()
    {
        ImGui::PopStyleColor();
    }

    ScopedDisabled::ScopedDisabled(bool disabled, const char* reasonTooltip)
        : m_disabled(disabled), m_reason(reasonTooltip)
    {
        if (m_disabled)
        {
            ImGui::BeginDisabled(true);
        }
    }

    ScopedDisabled::~ScopedDisabled()
    {
        if (m_disabled)
        {
            ImGui::EndDisabled();
            ItemTooltip(m_reason);
        }
    }

    ScopedPrimaryButtonColors::ScopedPrimaryButtonColors()
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ColV(Role::AccentFill));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColV(Role::AccentFillHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ColV(Role::AccentFillActive));
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextOnAccent));
    }

    ScopedPrimaryButtonColors::~ScopedPrimaryButtonColors()
    {
        ImGui::PopStyleColor(4);
    }

    // --- text --------------------------------------------------------------------------------

    void TextRole(Role r, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(r));
        ImGui::TextV(fmt, args);
        ImGui::PopStyleColor();
        va_end(args);
    }

    void SecondaryText(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextSecondary));
        ImGui::TextV(fmt, args);
        ImGui::PopStyleColor();
        va_end(args);
    }

    void MonoText(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        {
            ScopedFont mono(FontRole::Mono);
            ImGui::TextV(fmt, args);
        }
        va_end(args);
    }

    void StatusText(Status s, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(StatusTextRole(s)));
        ImGui::TextUnformatted(StatusGlyph(s));
        ImGui::SameLine(0.0f, Em(0.4f));
        ImGui::TextV(fmt, args);
        ImGui::PopStyleColor();
        va_end(args);
    }

    void KeyValue(const char* key, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        const FormatBuffer value = FormatV(fmt, args);
        va_end(args);
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextSecondary));
        ImGui::TextUnformatted(key != nullptr ? key : "");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, Em(0.6f));
        ScopedFont mono(FontRole::Mono);
        ImGui::TextUnformatted(value.text);
    }

    void HelpMarker(const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextSecondary));
        ImGui::TextUnformatted(Glyph::kCircleQuestion);
        ImGui::PopStyleColor();
        ItemTooltip(text);
    }

    void KeyHint(const char* keys)
    {
        ScopedFont smallFont(FontRole::Small);
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextSecondary));
        ImGui::TextUnformatted(keys != nullptr ? keys : "");
        ImGui::PopStyleColor();
    }

    // --- structure ---------------------------------------------------------------------------

    bool SectionHeader(const char* label, const char* icon, bool defaultOpen)
    {
        char text[256];
        if (icon != nullptr && icon[0] != '\0')
        {
            std::snprintf(text, sizeof(text), "%s  %s###%s", icon, label != nullptr ? label : "",
                          label != nullptr ? label : "");
        }
        else
        {
            std::snprintf(text, sizeof(text), "%s", label != nullptr ? label : "");
        }
        ScopedFont strong(FontRole::Strong);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (defaultOpen)
        {
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }
        return ImGui::CollapsingHeader(text, flags);
    }

    void SubHeader(const char* label)
    {
        ScopedFont strong(FontRole::Strong);
        ImGui::SeparatorText(label != nullptr ? label : "");
    }

    bool BeginPropertyGrid(const char* id, float labelFraction, float labelMinEm)
    {
        Detail::RuntimeState* state = Detail::CurrentState();
        if (state == nullptr)
        {
            return false;
        }
        const float available = ImGui::GetContentRegionAvail().x;
        if (available < Em(kPropertyGridStackEm))
        {
            ImGui::PushID(id);
            state->grids.push_back(Detail::PropertyGridFrame{true});
            return true;
        }
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
        {
            return false;
        }
        const float labelWidth = std::max(available * std::clamp(labelFraction, 0.1f, 0.9f), Em(labelMinEm));
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        state->grids.push_back(Detail::PropertyGridFrame{false});
        return true;
    }

    void PropertyLabel(const char* label, const char* tooltip)
    {
        Detail::RuntimeState* state = Detail::CurrentState();
        const bool stacked = state == nullptr || state->grids.empty() || state->grids.back().stacked;
        if (!stacked)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextSecondary));
        ImGui::TextUnformatted(label != nullptr ? label : "");
        ImGui::PopStyleColor();
        ItemTooltip(tooltip);
        if (!stacked)
        {
            ImGui::TableSetColumnIndex(1);
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    void EndPropertyGrid()
    {
        Detail::RuntimeState* state = Detail::CurrentState();
        if (state == nullptr || state->grids.empty())
        {
            return;
        }
        const bool stacked = state->grids.back().stacked;
        state->grids.pop_back();
        if (stacked)
        {
            ImGui::PopID();
        }
        else
        {
            ImGui::EndTable();
        }
    }

    bool BeginDataTable(const char* id, int columns, ImGuiTableFlags extra)
    {
        return ImGui::BeginTable(id, columns,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
                                     ImGuiTableFlags_ScrollY | extra);
    }

    void TableNumber(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        const FormatBuffer value = FormatV(fmt, args);
        va_end(args);
        ScopedFont mono(FontRole::Mono);
        const float width = ImGui::CalcTextSize(value.text).x;
        const float available = ImGui::GetContentRegionAvail().x;
        if (available > width)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
        }
        ImGui::TextUnformatted(value.text);
    }

    void VerticalSeparator()
    {
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    }

    // --- panel toolbar -----------------------------------------------------------------------

    bool BeginPanelToolbar(const char* id)
    {
        const float height = ImGui::GetFrameHeight() + Px(4.0f) * 2.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ColV(Role::SurfaceSunken));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px2(6.0f, 4.0f));
        const bool visible = ImGui::BeginChild(id, ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (!visible)
        {
            ImGui::EndChild();
        }
        return visible;
    }

    void EndPanelToolbar()
    {
        ImGui::EndChild();
    }

    bool SearchField(const char* id, char* buf, std::size_t bufSize, const char* hint)
    {
        char hintText[160];
        JoinIconLabel(hintText, sizeof(hintText), Glyph::kMagnifyingGlass, hint);
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool changed = ImGui::InputTextWithHint("##search", hintText, buf, bufSize);
        ImGui::PopID();
        return changed;
    }

    bool SearchField(const char* id, ImGuiTextFilter& filter, const char* hint)
    {
        if (SearchField(id, filter.InputBuf, sizeof(filter.InputBuf), hint))
        {
            filter.Build();
            return true;
        }
        return false;
    }

    bool IconButton(const char* id, const char* icon, const char* tooltip, bool selected, bool enabled)
    {
        const float side = std::max(Px(24.0f), ImGui::GetFrameHeight());
        ImGui::PushID(id);
        if (!enabled)
        {
            ImGui::BeginDisabled(true);
        }
        if (selected)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ColV(Role::SurfaceSelected));
            ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::AccentText));
        }
        const bool clicked = ImGui::Button(icon != nullptr ? icon : "", ImVec2(side, side));
        if (selected)
        {
            ImGui::PopStyleColor(2);
        }
        if (!enabled)
        {
            ImGui::EndDisabled();
        }
        ItemTooltip(tooltip);
        ImGui::PopID();
        return clicked && enabled;
    }

    bool SegmentedControl(const char* id, const char* const* labels, int count, int* current)
    {
        if (labels == nullptr || current == nullptr || count <= 0)
        {
            return false;
        }
        bool changed = false;
        ImGui::PushID(id);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
        for (int i = 0; i < count; ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine();
            }
            const bool selected = *current == i;
            if (selected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ColV(Role::SurfaceSelected));
                ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextPrimary));
            }
            ImGui::PushID(i);
            if (ImGui::Button(labels[i] != nullptr ? labels[i] : "") && !selected)
            {
                *current = i;
                changed = true;
            }
            ImGui::PopID();
            if (selected)
            {
                ImGui::PopStyleColor(2);
            }
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        return changed;
    }

    void ToolbarSeparator()
    {
        ImGui::SameLine();
        VerticalSeparator();
        ImGui::SameLine();
    }

    // --- buttons -----------------------------------------------------------------------------

    bool PrimaryButton(const char* label, ImVec2 size)
    {
        ScopedPrimaryButtonColors colours;
        return ImGui::Button(label, size);
    }

    bool SecondaryButton(const char* label, ImVec2 size)
    {
        return ImGui::Button(label, size);
    }

    bool DangerButton(const char* label, ImVec2 size)
    {
        // Hover and press tint the opaque ErrorFill instead of laying a translucent colour over
        // whatever surface is behind the button, so the audited label contrast holds everywhere.
        const Tokens& tokens = ActiveTokens();
        const float alpha = ImGui::GetStyle().Alpha;
        ImVec4 hovered = ToImVec4(DangerTintFill(tokens, kDangerHoverTint));
        ImVec4 pressed = ToImVec4(DangerTintFill(tokens, kDangerPressTint));
        hovered.w *= alpha;
        pressed.w *= alpha;
        ImGui::PushStyleColor(ImGuiCol_Button, ColV(Role::ErrorFill));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, pressed);
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::ErrorText));
        const bool clicked = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        return clicked;
    }

    // --- feedback ----------------------------------------------------------------------------

    void StatusChip(Status s, const char* label, const char* tooltip)
    {
        char text[256];
        JoinIconLabel(text, sizeof(text), StatusGlyph(s), label);
        Pill(text, StatusFillRole(s), StatusTextRole(s), StatusTextRole(s), -1.0f);
        ItemTooltip(tooltip);
    }

    void Badge(const char* label, Status s)
    {
        ScopedFont smallFont(FontRole::Small);
        Pill(label != nullptr ? label : "", StatusFillRole(s), StatusTextRole(s), Role::BorderSubtle, Px(3.0f));
    }

    bool Banner(Status s, const char* message, bool dismissible)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 padding(Px(10.0f), Px(8.0f));
        const float width = ImGui::GetContentRegionAvail().x;
        char lead[96];
        std::snprintf(lead, sizeof(lead), "%s  %s", StatusGlyph(s), StatusWord(s));
        const float leadWidth = ImGui::CalcTextSize(lead).x + Em(0.6f);
        const float closeWidth = dismissible ? ImGui::GetFrameHeight() + style.ItemInnerSpacing.x : 0.0f;
        const float wrapWidth = std::max(Em(4.0f), width - padding.x * 2.0f - leadWidth - closeWidth);
        const ImVec2 messageSize = ImGui::CalcTextSize(message != nullptr ? message : "", nullptr, false, wrapWidth);
        const float height = std::max(messageSize.y, ImGui::GetFrameHeight()) + padding.y * 2.0f;

        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + height);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float rounding = Px(ActiveTokens().metrics.rFrame);
        drawList->AddRectFilled(min, max, Col(StatusFillRole(s)), rounding);
        drawList->AddRectFilled(min, ImVec2(min.x + Px(3.0f), max.y), Col(StatusTextRole(s)), rounding,
                                ImDrawFlags_RoundCornersLeft);

        const float textY =
            min.y + padding.y + (std::max(messageSize.y, ImGui::GetFrameHeight()) - messageSize.y) * 0.5f;
        drawList->AddText(ImVec2(min.x + padding.x, textY), Col(StatusTextRole(s)), lead);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + padding.x + leadWidth, textY),
                          Col(Role::TextPrimary), message != nullptr ? message : "", nullptr, wrapWidth);

        bool dismissed = false;
        if (dismissible)
        {
            const float side = ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(ImVec2(max.x - padding.x - side, min.y + (height - side) * 0.5f));
            ImGui::PushID(message != nullptr ? message : "banner");
            dismissed = IconButton("dismiss", Glyph::kXmark, "Dismiss");
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(min);
        ImGui::Dummy(ImVec2(width, height));
        return dismissed;
    }

    bool EmptyState(const char* icon, const char* title, const char* hint, const char* actionLabel)
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const auto centered = [&](float itemWidth)
        {
            if (width > itemWidth)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - itemWidth) * 0.5f);
            }
        };
        ImGui::Dummy(ImVec2(0.0f, Em(1.5f)));
        if (icon != nullptr && icon[0] != '\0')
        {
            ScopedFont large(FontRole::H1, FontSize(FontRole::H1) * 1.6f);
            centered(ImGui::CalcTextSize(icon).x);
            ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextSecondary));
            ImGui::TextUnformatted(icon);
            ImGui::PopStyleColor();
        }
        if (title != nullptr)
        {
            ScopedFont strong(FontRole::Strong);
            centered(ImGui::CalcTextSize(title).x);
            ImGui::TextUnformatted(title);
        }
        if (hint != nullptr)
        {
            const float wrap = std::min(width, Em(28.0f));
            const ImVec2 hintSize = ImGui::CalcTextSize(hint, nullptr, false, wrap);
            centered(hintSize.x);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
            ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextSecondary));
            ImGui::TextUnformatted(hint);
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
        bool clicked = false;
        if (actionLabel != nullptr)
        {
            ImGui::Dummy(ImVec2(0.0f, Em(0.5f)));
            const ImGuiStyle& style = ImGui::GetStyle();
            centered(ImGui::CalcTextSize(actionLabel, nullptr, true).x + style.FramePadding.x * 2.0f);
            clicked = PrimaryButton(actionLabel);
        }
        return clicked;
    }

    void UnavailableFeature(const char* feature, const char* reason)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ColV(Role::TextSecondary));
        ImGui::TextUnformatted(Glyph::kLock);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, Em(0.5f));
        {
            ScopedFont strong(FontRole::Strong);
            ImGui::Text("%s is unavailable", feature != nullptr ? feature : "This feature");
        }
        if (reason != nullptr)
        {
            ImGui::PushTextWrapPos(0.0f);
            SecondaryText("%s", reason);
            ImGui::PopTextWrapPos();
        }
    }

    void ProgressIndeterminate(const char* id, ImVec2 size)
    {
        ImGui::PushID(id);
        const float width = size.x > 0.0f ? size.x : ImGui::GetContentRegionAvail().x;
        const float height = size.y > 0.0f ? size.y : Px(4.0f);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + height);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float rounding = height * 0.5f;
        drawList->AddRectFilled(min, max, Col(Role::SurfaceSunken), rounding);
        if (ReduceMotion())
        {
            // Static stripes: conveys "working" without movement.
            const float stripe = std::max(Px(6.0f), height * 2.0f);
            drawList->PushClipRect(min, max, true);
            for (float x = min.x - height; x < max.x; x += stripe * 2.0f)
            {
                drawList->AddQuadFilled(ImVec2(x, max.y), ImVec2(x + height, min.y), ImVec2(x + height + stripe, min.y),
                                        ImVec2(x + stripe, max.y), Col(Role::AccentFill, 0.55f));
            }
            drawList->PopClipRect();
        }
        else
        {
            const float segment = width * 0.3f;
            const float period = 1.4f;
            const float phase = std::fmod(static_cast<float>(ImGui::GetTime()), period) / period;
            const float start = min.x - segment + (width + segment) * phase;
            drawList->PushClipRect(min, max, true);
            drawList->AddRectFilled(ImVec2(start, min.y), ImVec2(start + segment, max.y), Col(Role::AccentFill),
                                    rounding);
            drawList->PopClipRect();
        }
        ImGui::Dummy(ImVec2(width, height));
        ImGui::PopID();
    }

    bool ConfirmDestructive(const char* popupId, const char* title, const char* body, const char* confirmLabel)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bool confirmed = false;
        if (ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            {
                ScopedFont strong(FontRole::H2);
                ImGui::TextUnformatted(title != nullptr ? title : "");
            }
            if (body != nullptr)
            {
                ImGui::PushTextWrapPos(Em(28.0f));
                ImGui::TextUnformatted(body);
                ImGui::PopTextWrapPos();
            }
            ImGui::Dummy(ImVec2(0.0f, Px(8.0f)));
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            const bool cancelled = SecondaryButton("Cancel");
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            confirmed = DangerButton(confirmLabel != nullptr ? confirmLabel : "Delete");
            if (cancelled || confirmed || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        return confirmed;
    }

    void InstallFocusRing(ImGuiContext* ctx)
    {
        if (ctx == nullptr)
        {
            return;
        }
        const ImGuiID owner = FocusRingOwnerId();
        for (const ImGuiContextHook& hook : ctx->Hooks)
        {
            if (hook.Owner == owner && hook.Type == ImGuiContextHookType_Shutdown)
            {
                return;
            }
        }
        // The context owns the state from here on; DestroyFocusRingState frees it at shutdown.
        FocusRingState* state = std::make_unique<FocusRingState>().release();
        const auto add = [&](ImGuiContextHookType type, ImGuiContextHookCallback callback)
        {
            ImGuiContextHook hook;
            hook.Type = type;
            hook.Owner = owner;
            hook.Callback = callback;
            hook.UserData = state;
            ImGui::AddContextHook(ctx, &hook);
        };
        add(ImGuiContextHookType_NewFramePost, &HideNativeNavCursor);
        add(ImGuiContextHookType_EndFramePre, &DrawTokenFocusRing);
        add(ImGuiContextHookType_Shutdown, &DestroyFocusRingState);
    }

    // --- brand -------------------------------------------------------------------------------

    BrandBandStyle ChooseBrandBandStyle(const Tokens& t) noexcept
    {
        std::array<Rgba, std::size(kBrandGradientStops)> stops{};
        for (std::size_t i = 0; i < stops.size(); ++i)
        {
            stops[i] = Hex(kBrandGradientStops[i]);
        }
        constexpr std::array kInks = {Role::TextOnAccent, Role::TextPrimary};
        BrandBandStyle style;
        style.ink = MostReadableRole(t, stops, kInks, &style.worstContrast);
        if (style.worstContrast < TextContrastMinimum(t.appearance))
        {
            style.gradient = false;
            style.ink = Role::TextPrimary;
            style.worstContrast = ContrastRatio(Get(t, Role::TextPrimary), Get(t, Role::SurfaceRaised));
        }
        return style;
    }

    // --- cards -------------------------------------------------------------------------------

    bool BeginCard(const char* id, ImVec2 size, bool selected)
    {
        const Metrics& metrics = ActiveTokens().metrics;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ColV(selected ? Role::SurfaceSelected : Role::SurfaceRaised));
        ImGui::PushStyleColor(ImGuiCol_Border, ColV(selected ? Role::AccentFill : Role::BorderSubtle));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(metrics.rCard));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, selected ? Px(2.0f) : Px(1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px2(12.0f, 10.0f));
        const bool visible = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        const bool clicked = visible && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                             ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (Detail::RuntimeState* state = Detail::CurrentState())
        {
            state->cardClicked.push_back(clicked);
        }
        return visible;
    }

    bool IsCardClicked()
    {
        const Detail::RuntimeState* state = Detail::CurrentState();
        return state != nullptr && !state->cardClicked.empty() && state->cardClicked.back();
    }

    void EndCard()
    {
        if (Detail::RuntimeState* state = Detail::CurrentState(); state != nullptr && !state->cardClicked.empty())
        {
            state->cardClicked.pop_back();
        }
        ImGui::EndChild();
    }
} // namespace Spark::Ui
