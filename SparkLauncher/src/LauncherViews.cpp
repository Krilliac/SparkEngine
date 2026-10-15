/**
 * @file LauncherViews.cpp
 * @brief SparkLauncher drawing: Ember Signal style install, left rail, Projects and New Project pages.
 */

#include "LauncherViews.h"

#include "LauncherApp.h"
#include "LauncherPaths.h"

#include "SparkUI/Brand.h"
#include "SparkUI/Fonts.h"
#include "SparkUI/Glyphs.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef SPARK_LAUNCHER_VERSION
#error "SPARK_LAUNCHER_VERSION must be supplied by the build system"
#endif

namespace SparkLauncher
{
    namespace
    {
        namespace Ui = Spark::Ui;
        using Ui::FontRole;
        using Ui::Role;
        using Ui::Status;

        /// Font Awesome 6 Free Solid glyphs the launcher draws beyond the kit's own (SparkUI/Glyphs.h).
        namespace Icon
        {
            constexpr const char* kFolderOpen = "\xef\x81\xbc"; ///< U+F07C
            constexpr const char* kPlus = "\xef\x81\xa7";       ///< U+F067
            constexpr const char* kRotate = "\xef\x8b\xb1";     ///< U+F2F1
            constexpr const char* kCube = "\xef\x86\xb2";       ///< U+F1B2
            constexpr const char* kCrosshairs = "\xef\x81\x9b"; ///< U+F05B
            constexpr const char* kGlobe = "\xef\x82\xac";      ///< U+F0AC
            constexpr const char* kUsers = "\xef\x83\x80";      ///< U+F0C0
            constexpr const char* kRunning = "\xef\x9c\x8c";    ///< U+F70C (person-running)
            constexpr const char* kDragon = "\xef\x9b\x95";     ///< U+F6D5
            constexpr const char* kCompass = "\xef\x85\x8e";    ///< U+F14E
            constexpr const char* kBolt = "\xef\x83\xa7";       ///< U+F0E7
        } // namespace Icon

        // Menu and button labels (glyph + two spaces + words, the kit's icon-label spacing).
        constexpr const char* kMenuPlay = "\xef\x84\x9b  Play";                           // U+F11B gamepad
        constexpr const char* kMenuServer = "\xef\x88\xb3  Dedicated Server";             // U+F233 server
        constexpr const char* kMenuServices = "\xef\x9b\xbf  Service Topology";           // U+F6FF network-wired
        constexpr const char* kMenuCopyPath = "\xef\x83\x85  Copy Path";                  // U+F0C5 copy
        constexpr const char* kMenuRemove = "\xef\x8b\xad  Remove from list\xe2\x80\xa6"; // U+F2ED trash-can
        constexpr const char* kOpenExistingLabel = "Open Existing\xe2\x80\xa6";
        constexpr const char* kNewProjectLabel = "\xef\x81\xa7  New Project"; // U+F067 plus
        constexpr const char* kLocateLabel = "Locate\xe2\x80\xa6";
        constexpr const char* kRemoveLabel = "Remove\xe2\x80\xa6";
        constexpr const char* kBrowseLabel = "Browse\xe2\x80\xa6";
        constexpr const char* kRemovalPopupId = "##ConfirmRemoveRecentProject";

        // Logical (100 %) layout metrics of the launcher (spec section 4.3).
        constexpr float kComfortableBodyPx = 16.0f;
        constexpr float kRailWidthEm = 15.0f;
        constexpr float kRailHeaderHeight = 72.0f;
        constexpr float kRailLogoSize = 32.0f;
        constexpr float kRailMargin = 16.0f;
        constexpr float kNavRowHeight = 40.0f;
        constexpr float kNavRowInset = 8.0f;
        constexpr float kNavRowGap = 2.0f;
        constexpr float kNavAccentBarWidth = 3.0f;
        constexpr float kProjectRowHeightEm = 3.6f;
        constexpr float kProjectRowPadding = 12.0f;
        constexpr float kProjectMetaWidthEm = 9.0f;
        constexpr float kCardWidthEm = 15.0f;
        constexpr float kCardHeightEm = 7.0f;
        constexpr float kCardItemSpacingY = 4.0f;
        constexpr float kSectionGap = 12.0f;
        constexpr int kCardDescriptionLines = 2;
        constexpr auto kSuccessBannerLifetime = std::chrono::seconds(6);
        constexpr std::string_view kEllipsis = "...";

        /// Byte offset of every UTF-8 code point start in @p text.
        std::vector<std::size_t> CodepointStarts(std::string_view text)
        {
            std::vector<std::size_t> starts;
            starts.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if ((static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u)
                {
                    starts.push_back(i);
                }
            }
            return starts;
        }

        float TextWidth(std::string_view text)
        {
            return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
        }

        /// @p text shortened in the middle ("C:/Users/.../Game.sparkproject") to fit @p maxWidth in
        /// the current font. Cuts only at code point boundaries.
        std::string MiddleEllipsis(std::string_view text, float maxWidth)
        {
            if (TextWidth(text) <= maxWidth)
            {
                return std::string(text);
            }
            const std::vector<std::size_t> starts = CodepointStarts(text);
            const std::size_t count = starts.size();
            const auto build = [&](std::size_t keep)
            {
                const std::size_t head = (keep + 1) / 2;
                const std::size_t tail = keep / 2;
                std::string out(text.substr(0, head < count ? starts[head] : text.size()));
                out += kEllipsis;
                out += text.substr(tail == 0 ? text.size() : starts[count - tail]);
                return out;
            };
            // Largest number of kept code points whose shortened form still fits.
            std::size_t low = 0;
            std::size_t high = count;
            while (low < high)
            {
                const std::size_t mid = (low + high + 1) / 2;
                if (TextWidth(build(mid)) <= maxWidth)
                {
                    low = mid;
                }
                else
                {
                    high = mid - 1;
                }
            }
            return build(low);
        }

        /// @p text cut at a code point boundary (with "...") so it wraps to at most @p maxLines
        /// lines of @p wrapWidth in the current font.
        std::string ClampToLines(std::string_view text, float wrapWidth, int maxLines)
        {
            const float limit = ImGui::GetTextLineHeight() * static_cast<float>(maxLines) + 0.5f;
            const auto height = [wrapWidth](std::string_view candidate)
            { return ImGui::CalcTextSize(candidate.data(), candidate.data() + candidate.size(), false, wrapWidth).y; };
            if (height(text) <= limit)
            {
                return std::string(text);
            }
            const std::vector<std::size_t> starts = CodepointStarts(text);
            const auto build = [&](std::size_t keep)
            {
                std::string out(text.substr(0, starts[keep]));
                out += kEllipsis;
                return out;
            };
            std::size_t low = 0;
            std::size_t high = starts.size() - 1;
            while (low < high)
            {
                const std::size_t mid = (low + high + 1) / 2;
                if (height(build(mid)) <= limit)
                {
                    low = mid;
                }
                else
                {
                    high = mid - 1;
                }
            }
            return build(low);
        }

        char FoldAscii(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        /// ASCII case-insensitive substring test; non-ASCII bytes must match exactly.
        bool ContainsFolded(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty())
            {
                return true;
            }
            const auto match = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                           [](char a, char b) { return FoldAscii(a) == FoldAscii(b); });
            return match != haystack.end();
        }

        /// Local date and time of @p epochSeconds ("2026-10-14 09:30"), or "never".
        std::string FormatTimestamp(std::uint64_t epochSeconds)
        {
            if (epochSeconds == 0)
            {
                return "never";
            }
            const auto t = static_cast<std::time_t>(epochSeconds);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[64] = {};
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
            return buf;
        }

        /// "Opened 2 h ago" style age of @p epochSeconds relative to @p now.
        std::string OpenedAgo(std::uint64_t epochSeconds, std::time_t now)
        {
            if (epochSeconds == 0)
            {
                return "Never opened";
            }
            const double seconds = std::difftime(now, static_cast<std::time_t>(epochSeconds));
            if (seconds < 60.0)
            {
                return "Opened just now";
            }
            const auto minutes = static_cast<long long>(seconds / 60.0);
            if (minutes < 60)
            {
                return "Opened " + std::to_string(minutes) + " min ago";
            }
            const long long hours = minutes / 60;
            if (hours < 24)
            {
                return "Opened " + std::to_string(hours) + " h ago";
            }
            const long long days = hours / 24;
            if (days == 1)
            {
                return "Opened yesterday";
            }
            if (days < 30)
            {
                return "Opened " + std::to_string(days) + " days ago";
            }
            return "Opened " + FormatTimestamp(epochSeconds).substr(0, 10);
        }

        /// Genre glyph of a template card; unknown genres use the cube.
        const char* GenreIcon(std::string_view genre)
        {
            struct GenreGlyph
            {
                std::string_view genre;
                const char* glyph;
            };
            constexpr GenreGlyph kGenres[] = {
                {"fps", Icon::kCrosshairs},     {"mmo", Icon::kGlobe},    {"multiplayer arena", Icon::kUsers},
                {"platformer", Icon::kRunning}, {"rpg", Icon::kDragon},   {"adventure", Icon::kCompass},
                {"action", Icon::kBolt},        {"general", Icon::kCube},
            };
            for (const GenreGlyph& entry : kGenres)
            {
                if (entry.genre.size() == genre.size() && ContainsFolded(genre, entry.genre))
                {
                    return entry.glyph;
                }
            }
            return Icon::kCube;
        }

        float ButtonWidth(const char* label)
        {
            return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        }

        float IconButtonSide()
        {
            return std::max(Ui::Px(24.0f), ImGui::GetFrameHeight());
        }

        void HoverTooltip(const char* text)
        {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGui::SetTooltip("%s", text);
            }
        }

        // --- rail ---------------------------------------------------------------------------------

        /// Brand band: ember gradient (flat in High Contrast), logomark, product name and version, all in
        /// the ink that measures most readable on the band (Ui::ChooseBrandBandStyle).
        void DrawRailHeader(float width)
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float height = Ui::Px(kRailHeaderHeight);
            const float margin = Ui::Px(kRailMargin);
            const float logo = Ui::Px(kRailLogoSize);
            const Ui::BrandBandStyle band = Ui::ChooseBrandBandStyle(Ui::ActiveTokens());
            const ImVec2 bandMax(origin.x + width, origin.y + height);
            if (band.gradient)
            {
                Ui::DrawBrandGradient(drawList, origin, bandMax);
            }
            else
            {
                drawList->AddRectFilled(origin, bandMax, Ui::Col(Role::SurfaceRaised));
            }
            Ui::DrawLogomark(drawList, ImVec2(origin.x + margin + logo * 0.5f, origin.y + height * 0.5f), logo, false,
                             Ui::Col(band.ink));

            float titleHeight = 0.0f;
            float subtitleHeight = 0.0f;
            {
                Ui::ScopedFont title(FontRole::H1);
                titleHeight = ImGui::GetFontSize();
            }
            {
                Ui::ScopedFont subtitle(FontRole::MonoSmall);
                subtitleHeight = ImGui::GetFontSize();
            }
            const float gap = Ui::Px(2.0f);
            const float textX = origin.x + margin + logo + Ui::Px(12.0f);
            const float textY = origin.y + (height - titleHeight - gap - subtitleHeight) * 0.5f;
            Ui::ScopedRoleColor ink(ImGuiCol_Text, band.ink);
            {
                Ui::ScopedFont title(FontRole::H1);
                ImGui::SetCursorScreenPos(ImVec2(textX, textY));
                ImGui::TextUnformatted("Spark Engine");
            }
            {
                Ui::ScopedFont subtitle(FontRole::MonoSmall);
                ImGui::SetCursorScreenPos(ImVec2(textX, textY + titleHeight + gap));
                ImGui::TextUnformatted("Launcher \xc2\xb7 " SPARK_LAUNCHER_VERSION);
            }
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height + Ui::Px(kSectionGap)));
        }

        /// One navigation row; selected = raised fill, ember left bar and accent text.
        bool NavRow(const char* id, const char* icon, const char* label, const char* trailing, bool selected)
        {
            const float inset = Ui::Px(kNavRowInset);
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const ImVec2 min(cursor.x + inset, cursor.y);
            const float width = ImGui::GetContentRegionAvail().x - inset * 2.0f;
            const float height = Ui::Px(kNavRowHeight);
            const ImVec2 max(min.x + width, min.y + height);

            ImGui::SetCursorScreenPos(min);
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
            const bool hovered = ImGui::IsItemHovered();

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const float rounding = Ui::Px(Ui::ActiveTokens().metrics.rFrame);
            if (selected)
            {
                drawList->AddRectFilled(min, max, Ui::Col(Role::SurfaceRaised), rounding);
                drawList->AddRectFilled(min, ImVec2(min.x + Ui::Px(kNavAccentBarWidth), max.y),
                                        Ui::Col(Role::AccentFill), rounding, ImDrawFlags_RoundCornersLeft);
            }
            else if (hovered)
            {
                drawList->AddRectFilled(min, max, Ui::Col(Role::SurfaceHover), rounding);
            }
            const ImU32 ink = Ui::Col(selected ? Role::AccentText : Role::TextPrimary);
            const float textY = min.y + (height - ImGui::GetFontSize()) * 0.5f;
            const float iconX = min.x + Ui::Px(kRailMargin);
            drawList->AddText(ImVec2(iconX, textY), ink, icon);
            drawList->AddText(ImVec2(iconX + Ui::Em(1.6f), textY), ink, label);
            if (trailing != nullptr)
            {
                drawList->AddText(ImVec2(max.x - Ui::Px(kProjectRowPadding) - TextWidth(trailing), textY),
                                  Ui::Col(Role::TextSecondary), trailing);
            }
            return pressed;
        }

        /// Engine install directory pinned to the bottom of the rail.
        void DrawRailFooter(const std::string& engineDirectory, float width)
        {
            const float margin = Ui::Px(kRailMargin);
            float labelHeight = 0.0f;
            float pathHeight = 0.0f;
            {
                Ui::ScopedFont small(FontRole::Small);
                labelHeight = ImGui::GetTextLineHeightWithSpacing();
            }
            {
                Ui::ScopedFont mono(FontRole::MonoSmall);
                pathHeight = ImGui::GetTextLineHeight();
            }
            const ImVec2 windowPos = ImGui::GetWindowPos();
            const float top = windowPos.y + ImGui::GetWindowHeight() - margin - labelHeight - pathHeight;
            if (top < ImGui::GetCursorScreenPos().y)
            {
                return; // Window too short: the navigation keeps the space.
            }
            ImGui::SetCursorScreenPos(ImVec2(windowPos.x + margin, top));
            {
                Ui::ScopedFont small(FontRole::Small);
                Ui::SecondaryText("Engine");
            }
            ImGui::SetCursorScreenPos(ImVec2(windowPos.x + margin, top + labelHeight));
            Ui::ScopedFont mono(FontRole::MonoSmall);
            Ui::ScopedRoleColor ink(ImGuiCol_Text, Role::TextSecondary);
            const std::string shown = MiddleEllipsis(engineDirectory, width - margin * 2.0f);
            ImGui::TextUnformatted(shown.c_str());
            HoverTooltip(engineDirectory.c_str());
        }

        void DrawRail(LauncherApp& app)
        {
            const float width = Ui::Em(kRailWidthEm);
            {
                Ui::ScopedRoleColor background(ImGuiCol_ChildBg, Role::SurfaceSunken);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::BeginChild("##LauncherRail", ImVec2(width, 0.0f), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleVar();
            }
            DrawRailHeader(width);

            char count[32];
            std::snprintf(count, sizeof(count), "%zu", app.RecentProjects().size());
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, Ui::Px(kNavRowGap)));
            if (NavRow("##NavProjects", Icon::kFolderOpen, "Projects", count,
                       app.ActiveView() == LauncherView::Projects))
            {
                app.SetActiveView(LauncherView::Projects);
            }
            if (NavRow("##NavNewProject", Icon::kPlus, "New Project", nullptr,
                       app.ActiveView() == LauncherView::NewProject))
            {
                app.SetActiveView(LauncherView::NewProject);
            }
            ImGui::PopStyleVar();
            DrawRailFooter(app.EngineDirectory(), width);
            ImGui::EndChild();
        }

        // --- status ---------------------------------------------------------------------------------

        void DrawStatusBanner(LauncherApp& app)
        {
            if (app.StatusMessage().empty())
            {
                return;
            }
            const bool isError = app.StatusIsError();
            // Errors stay until dismissed; a success fades out of the way unless motion is reduced,
            // in which case it also waits for the user.
            if (!isError && !Ui::ReduceMotion() &&
                std::chrono::steady_clock::now() - app.StatusTime() >= kSuccessBannerLifetime)
            {
                app.ClearStatus();
                return;
            }
            if (Ui::Banner(isError ? Status::Error : Status::Success, app.StatusMessage().c_str(), true))
            {
                app.ClearStatus();
            }
            ImGui::Dummy(ImVec2(0.0f, Ui::Px(kSectionGap)));
        }

        // --- projects page --------------------------------------------------------------------------

        /// What a project row asked for this frame; run after the list so the cache is not
        /// refreshed while it is being iterated.
        struct RowAction
        {
            enum class Kind
            {
                None,
                Launch,
                Locate
            };
            Kind kind = Kind::None;
            LaunchTarget target = LaunchTarget::Editor;
            SparkEditor::RecentProject project;
        };

        void RequestRemoval(LauncherViewState& state, const SparkEditor::RecentProject& project)
        {
            state.pendingRemovalPath = project.path;
            state.pendingRemovalName = project.name;
            state.openRemovalConfirm = true;
        }

        void DrawRemovalConfirm(LauncherApp& app)
        {
            LauncherViewState& state = app.ViewState();
            if (state.openRemovalConfirm)
            {
                ImGui::OpenPopup(kRemovalPopupId);
                state.openRemovalConfirm = false;
            }
            if (!ImGui::IsPopupOpen(kRemovalPopupId))
            {
                return;
            }
            const std::string title = "Remove '" + state.pendingRemovalName + "' from recent projects?";
            if (Ui::ConfirmDestructive(kRemovalPopupId, title.c_str(), "Files on disk are not touched.", "Remove"))
            {
                app.RemoveRecentProject(state.pendingRemovalPath);
                state.pendingRemovalPath.clear();
                state.pendingRemovalName.clear();
            }
        }

        void DrawProjectsHeader(LauncherApp& app)
        {
            const float top = ImGui::GetCursorPosY();
            float titleHeight = 0.0f;
            {
                Ui::ScopedFont title(FontRole::H1);
                titleHeight = ImGui::GetFontSize();
                ImGui::TextUnformatted("Projects");
            }
            char count[32];
            std::snprintf(count, sizeof(count), "%zu", app.RecentProjects().size());
            ImGui::SameLine(0.0f, Ui::Px(10.0f));
            float badgeHeight = 0.0f;
            {
                Ui::ScopedFont small(FontRole::Small);
                badgeHeight = ImGui::GetFontSize() + Ui::Px(2.0f) * 2.0f;
            }
            ImGui::SetCursorPosY(top + std::max(0.0f, (titleHeight - badgeHeight) * 0.5f));
            Ui::Badge(count);

            const ImGuiStyle& style = ImGui::GetStyle();
            const float actionsWidth =
                ButtonWidth(kOpenExistingLabel) + style.ItemSpacing.x + ButtonWidth(kNewProjectLabel);
            ImGui::SameLine();
            ImGui::SetCursorPos(
                ImVec2(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - actionsWidth),
                       top + std::max(0.0f, (titleHeight - ImGui::GetFrameHeight()) * 0.5f)));
            if (Ui::SecondaryButton(kOpenExistingLabel))
            {
                app.OpenExistingProject();
            }
            ImGui::SameLine();
            if (Ui::PrimaryButton(kNewProjectLabel))
            {
                app.SetActiveView(LauncherView::NewProject);
            }
            ImGui::SetCursorPosY(top + std::max(titleHeight, ImGui::GetFrameHeight()) + Ui::Px(kSectionGap));
        }

        void DrawProjectsToolbar(LauncherApp& app)
        {
            LauncherViewState& state = app.ViewState();
            const float side = IconButtonSide();
            if (ImGui::BeginTable("##ProjectsToolbar", 2,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
            {
                ImGui::TableSetupColumn("search", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("refresh", ImGuiTableColumnFlags_WidthFixed, side);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Ui::SearchField("##ProjectSearch", state.search.data(), state.search.size(), "Search by name or path");
                ImGui::TableNextColumn();
                if (Ui::IconButton("##Refresh", Icon::kRotate, "Refresh: re-check which projects still exist"))
                {
                    app.RefreshRecentProjects();
                }
                ImGui::EndTable();
            }
            ImGui::Dummy(ImVec2(0.0f, Ui::Px(4.0f)));
        }

        /// One two-line project row. Returns what the user asked for (or Kind::None).
        RowAction DrawProjectRow(const SparkEditor::RecentProject& project, LauncherViewState& state, std::time_t now)
        {
            RowAction action;
            const ImGuiStyle& style = ImGui::GetStyle();
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = Ui::Em(kProjectRowHeightEm);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + width, min.y + height);
            const float pad = Ui::Px(kProjectRowPadding);
            const bool rowHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max);

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->ChannelsSplit(2);
            drawList->ChannelsSetCurrent(1);

            // Columns, right to left: actions, metadata, then name and path in what is left.
            const float side = IconButtonSide();
            const float actionsWidth =
                project.valid ? ButtonWidth("Open") + style.ItemSpacing.x + side
                              : ButtonWidth(kLocateLabel) + style.ItemSpacing.x + ButtonWidth(kRemoveLabel);
            const float actionsX = max.x - pad - actionsWidth;
            const float metaWidth = Ui::Em(kProjectMetaWidthEm);
            const float metaX = actionsX - pad - metaWidth;
            const float textX = min.x + pad;
            const float textWidth = std::max(0.0f, metaX - pad - textX);

            float nameHeight = 0.0f;
            float pathHeight = 0.0f;
            {
                Ui::ScopedFont strong(FontRole::Strong);
                nameHeight = ImGui::GetFontSize();
            }
            {
                Ui::ScopedFont mono(FontRole::MonoSmall);
                pathHeight = ImGui::GetFontSize();
            }
            const float gap = Ui::Px(4.0f);
            const float top = min.y + (height - nameHeight - gap - pathHeight) * 0.5f;

            ImGui::SetCursorScreenPos(ImVec2(textX, top));
            {
                Ui::ScopedFont strong(FontRole::Strong);
                Ui::ScopedRoleColor ink(ImGuiCol_Text, project.valid ? Role::TextPrimary : Role::TextSecondary);
                const std::string name = MiddleEllipsis(project.name, textWidth);
                ImGui::TextUnformatted(name.c_str());
            }
            ImGui::SetCursorScreenPos(ImVec2(textX, top + nameHeight + gap));
            {
                Ui::ScopedFont mono(FontRole::MonoSmall);
                Ui::ScopedRoleColor ink(ImGuiCol_Text, Role::TextSecondary);
                const std::string path = MiddleEllipsis(project.path, textWidth);
                ImGui::TextUnformatted(path.c_str());
                HoverTooltip(project.path.c_str());
            }

            if (project.valid)
            {
                ImGui::SetCursorScreenPos(ImVec2(metaX, top));
                {
                    Ui::ScopedFont small(FontRole::Small);
                    Ui::SecondaryText("%s", OpenedAgo(project.lastOpened, now).c_str());
                }
                HoverTooltip(FormatTimestamp(project.lastOpened).c_str());
                if (!project.engineVersion.empty())
                {
                    ImGui::SetCursorScreenPos(ImVec2(metaX, top + nameHeight + gap));
                    Ui::Badge(project.engineVersion.c_str());
                    HoverTooltip("Engine version the project was last saved with");
                }
            }
            else
            {
                ImGui::SetCursorScreenPos(ImVec2(metaX, min.y + (height - ImGui::GetFrameHeight()) * 0.5f));
                Ui::StatusChip(Status::Error, "Missing",
                               "The project file no longer exists at this path. Locate it or remove it from the list.");
            }

            ImGui::SetCursorScreenPos(ImVec2(actionsX, min.y + (height - ImGui::GetFrameHeight()) * 0.5f));
            if (project.valid)
            {
                if (Ui::PrimaryButton("Open"))
                {
                    action.kind = RowAction::Kind::Launch;
                    action.target = LaunchTarget::Editor;
                }
                ImGui::SameLine();
                if (Ui::IconButton("##More", Ui::Glyph::kEllipsis, "More actions"))
                {
                    ImGui::OpenPopup("##ProjectActions");
                }
                if (ImGui::BeginPopup("##ProjectActions"))
                {
                    const auto launch = [&action](LaunchTarget target)
                    {
                        action.kind = RowAction::Kind::Launch;
                        action.target = target;
                    };
                    if (ImGui::MenuItem(kMenuPlay))
                    {
                        launch(LaunchTarget::Game);
                    }
                    if (ImGui::MenuItem(kMenuServer))
                    {
                        launch(LaunchTarget::DedicatedServer);
                    }
                    if (ImGui::MenuItem(kMenuServices))
                    {
                        launch(LaunchTarget::ServiceTopology);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(kMenuCopyPath))
                    {
                        ImGui::SetClipboardText(project.path.c_str());
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(kMenuRemove))
                    {
                        RequestRemoval(state, project);
                    }
                    ImGui::EndPopup();
                }
                // Double-clicking the row (not one of its buttons) opens the editor, like "Open".
                if (rowHovered && !ImGui::IsAnyItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    action.kind = RowAction::Kind::Launch;
                    action.target = LaunchTarget::Editor;
                }
            }
            else
            {
                if (Ui::SecondaryButton(kLocateLabel))
                {
                    action.kind = RowAction::Kind::Locate;
                }
                ImGui::SameLine();
                if (Ui::SecondaryButton(kRemoveLabel))
                {
                    RequestRemoval(state, project);
                }
            }

            drawList->ChannelsSetCurrent(0);
            const float rounding = Ui::Px(Ui::ActiveTokens().metrics.rCard);
            drawList->AddRectFilled(min, max, Ui::Col(rowHovered ? Role::SurfaceHover : Role::SurfaceRaised), rounding);
            drawList->AddRect(min, max, Ui::Col(Role::BorderSubtle), rounding, 0, std::max(1.0f, Ui::Px(1.0f)));
            drawList->ChannelsMerge();

            ImGui::SetCursorScreenPos(min);
            ImGui::Dummy(ImVec2(width, height));
            if (action.kind != RowAction::Kind::None)
            {
                action.project = project;
            }
            return action;
        }

        void DrawProjectsPage(LauncherApp& app)
        {
            DrawRemovalConfirm(app);
            DrawProjectsHeader(app);

            const std::vector<SparkEditor::RecentProject>& projects = app.RecentProjects();
            if (projects.empty())
            {
                if (Ui::EmptyState(Icon::kFolderOpen, "No recent projects",
                                   "Create a project from a template or open an existing folder.", "New Project"))
                {
                    app.SetActiveView(LauncherView::NewProject);
                }
                return;
            }

            DrawProjectsToolbar(app);
            LauncherViewState& state = app.ViewState();
            const std::string_view filter(state.search.data());
            const std::time_t now = std::time(nullptr);
            RowAction requested;
            std::size_t shown = 0;
            ImGui::BeginChild("##ProjectList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            for (const SparkEditor::RecentProject& project : projects)
            {
                if (!ContainsFolded(project.name, filter) && !ContainsFolded(project.path, filter))
                {
                    continue;
                }
                ++shown;
                ImGui::PushID(project.path.c_str());
                RowAction action = DrawProjectRow(project, state, now);
                if (action.kind != RowAction::Kind::None)
                {
                    requested = std::move(action);
                }
                ImGui::PopID();
            }
            if (shown == 0)
            {
                Ui::EmptyState(Ui::Glyph::kMagnifyingGlass, "No matching projects",
                               "No recent project name or path contains the search text.");
            }
            ImGui::EndChild();

            switch (requested.kind)
            {
            case RowAction::Kind::Launch:
                app.OpenRecentProject(requested.project, requested.target);
                break;
            case RowAction::Kind::Locate:
                app.LocateRecentProject(requested.project);
                break;
            case RowAction::Kind::None:
                break;
            }
        }

        // --- new project page -----------------------------------------------------------------------

        void DrawTemplateCard(LauncherApp& app, int index, ImVec2 size)
        {
            const TemplateEntry& tpl = app.Templates()[static_cast<std::size_t>(index)];
            const bool selected = index == app.SelectedTemplate();
            const ImVec2 origin = ImGui::GetCursorScreenPos();

            // Keyboard target under the card: navigation reaches it and Enter/Space selects. The kit's
            // focus ring (Ui::InstallFocusRing) is drawn just outside it, apart from the selection border.
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##Select", size))
            {
                app.SelectTemplate(index);
            }
            ImGui::SetCursorScreenPos(origin);

            Ui::BeginCard("##Card", size, selected);
            if (Ui::IsCardClicked())
            {
                app.SelectTemplate(index);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(ImGui::GetStyle().ItemSpacing.x, Ui::Px(kCardItemSpacingY)));
            const float contentWidth = ImGui::GetContentRegionAvail().x;
            float checkWidth = 0.0f;
            if (selected)
            {
                checkWidth = TextWidth(Ui::Glyph::kCircleCheck) + Ui::Px(6.0f);
                const ImVec2 cardMin = ImGui::GetWindowPos();
                const ImVec2 checkPos(cardMin.x + ImGui::GetWindowWidth() - Ui::Px(kProjectRowPadding) -
                                          TextWidth(Ui::Glyph::kCircleCheck),
                                      ImGui::GetCursorScreenPos().y);
                ImGui::GetWindowDrawList()->AddText(checkPos, Ui::Col(Role::AccentText), Ui::Glyph::kCircleCheck);
            }
            {
                Ui::ScopedRoleColor ink(ImGuiCol_Text, selected ? Role::AccentText : Role::TextSecondary);
                ImGui::TextUnformatted(GenreIcon(tpl.genre));
            }
            ImGui::SameLine();
            {
                Ui::ScopedFont strong(FontRole::Strong);
                const std::string title =
                    MiddleEllipsis(tpl.displayName, std::max(0.0f, ImGui::GetContentRegionAvail().x - checkWidth));
                ImGui::TextUnformatted(title.c_str());
            }
            {
                Ui::ScopedRoleColor ink(ImGuiCol_Text, Role::TextSecondary);
                const std::string description = ClampToLines(tpl.description, contentWidth, kCardDescriptionLines);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(description.c_str());
                ImGui::PopTextWrapPos();
            }
            if (!tpl.genre.empty())
            {
                Ui::Badge(tpl.genre.c_str());
                ImGui::SameLine();
            }
            if (!tpl.gameModule.empty())
            {
                Ui::Badge(tpl.gameModule.c_str());
                HoverTooltip("Game module");
            }
            ImGui::PopStyleVar();
            Ui::EndCard();
        }

        void DrawTemplateGrid(LauncherApp& app)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            const ImVec2 cardSize(Ui::Em(kCardWidthEm), Ui::Em(kCardHeightEm));
            const float available = ImGui::GetContentRegionAvail().x;
            const int columns =
                std::max(1, static_cast<int>((available + style.ItemSpacing.x) / (cardSize.x + style.ItemSpacing.x)));
            const int count = static_cast<int>(app.Templates().size());
            for (int index = 0; index < count; ++index)
            {
                if (index % columns != 0)
                {
                    ImGui::SameLine();
                }
                ImGui::PushID(index);
                DrawTemplateCard(app, index, cardSize);
                ImGui::PopID();
            }
        }

        void DrawNewProjectForm(LauncherApp& app)
        {
            NewProjectForm& form = app.Form();
            const NewProjectValidation validation = app.ValidateNewProject();
            const ImGuiStyle& style = ImGui::GetStyle();
            if (Ui::BeginPropertyGrid("##NewProjectForm"))
            {
                Ui::PropertyLabel("Name");
                ImGui::InputText("##Name", form.name, sizeof(form.name));
                if (!validation.nameError.empty())
                {
                    Ui::StatusText(Status::Error, "%s", validation.nameError.c_str());
                }

                Ui::PropertyLabel("Location");
                ImGui::SetNextItemWidth(-(ButtonWidth(kBrowseLabel) + style.ItemInnerSpacing.x));
                ImGui::InputText("##Location", form.location, sizeof(form.location));
                ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
                if (Ui::SecondaryButton(kBrowseLabel))
                {
                    app.BrowseNewProjectLocation();
                }
                if (!validation.locationError.empty())
                {
                    Ui::StatusText(Status::Error, "%s", validation.locationError.c_str());
                }

                Ui::PropertyLabel("Will be created at:");
                ImGui::PushTextWrapPos(0.0f);
                if (validation.projectRoot.empty())
                {
                    Ui::SecondaryText("Enter a name and location to see the project folder.");
                }
                else
                {
                    Ui::MonoText("%s", PathToUtf8(validation.projectRoot).c_str());
                }
                ImGui::PopTextWrapPos();
                Ui::EndPropertyGrid();
            }

            ImGui::Dummy(ImVec2(0.0f, Ui::Px(kSectionGap)));
            Ui::ScopedDisabled disabled(!validation.IsValid(), validation.FirstError().c_str());
            if (Ui::PrimaryButton("Create Project"))
            {
                app.CreateNewProject();
            }
        }

        void DrawNewProjectPage(LauncherApp& app)
        {
            {
                Ui::ScopedFont title(FontRole::H1);
                ImGui::TextUnformatted("New Project");
            }
            Ui::SecondaryText("Pick a template, name the project and choose where to create it.");
            ImGui::Dummy(ImVec2(0.0f, Ui::Px(kSectionGap)));

            if (app.Templates().empty())
            {
                Ui::Banner(Status::Error,
                           "No templates found in the development tree or installed SparkEngine share directory.");
                return;
            }
            Ui::SubHeader("Template");
            DrawTemplateGrid(app);
            ImGui::Dummy(ImVec2(0.0f, Ui::Px(kSectionGap)));
            Ui::SubHeader("Project");
            DrawNewProjectForm(app);
        }
    } // namespace

    void InstallLauncherStyle(float dpiScale, Spark::Ui::Appearance appearance)
    {
        ImGuiContext* context = ImGui::GetCurrentContext();
        if (context == nullptr)
        {
            return;
        }
        Ui::Tokens tokens = Ui::GenerateTokens(Ui::BuiltinSeed(Ui::PaletteId::Ember), appearance);
        // The Comfortable profile replaces the spacing only; the appearance's focus ring width
        // (3 px in High Contrast) stays.
        const float focusRing = tokens.metrics.focusRing;
        tokens.metrics = Ui::MetricsFor(Ui::Density::Standard, Ui::MetricsProfile::Comfortable);
        tokens.metrics.focusRing = focusRing;
        tokens.type.body = kComfortableBodyPx;
        Ui::Install(context, tokens, dpiScale, 1.0f, false);
        Ui::InstallFocusRing(context);

        const std::filesystem::path fontDirectory = Ui::FindFontDirectory(GetLauncherExecutablePath().parent_path());
        const Ui::FontSet fonts = Ui::LoadFontSet(*ImGui::GetIO().Fonts, fontDirectory, tokens.type.body);
        if (!fonts.diagnostic.empty())
        {
            std::fprintf(stderr, "SparkLauncher fonts: %s\n", fonts.diagnostic.c_str());
        }
    }

    void DrawLauncher(LauncherApp& app)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("SparkLauncher", nullptr, flags);
        ImGui::PopStyleVar();

        DrawRail(app);
        ImGui::SameLine(0.0f, 0.0f);

        // The content pane keeps the Comfortable window padding (24 x 20 logical).
        ImGui::BeginChild("##LauncherContent", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawStatusBanner(app);
        switch (app.ActiveView())
        {
        case LauncherView::Projects:
            DrawProjectsPage(app);
            break;
        case LauncherView::NewProject:
            DrawNewProjectPage(app);
            break;
        }
        ImGui::EndChild();

        ImGui::End();
    }
} // namespace SparkLauncher
