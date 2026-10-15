/**
 * @file WizardGui.cpp
 * @brief The Ember Signal installer wizard: step rail, pages, footer and the install worker.
 */

#include "WizardGui.h"

#include "InstallState.h"
#include "Installer.h"

#include "SparkUI/Brand.h"
#include "SparkUI/Fonts.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Tokens.h"
#include "SparkUI/Widgets.h"

#include <imgui.h>

#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace SparkInstaller
{
    namespace fs = std::filesystem;
    namespace ui = ::Spark::Ui;

    namespace
    {
        // The wizard's pages, in step-rail order. The Progress page is the rail's "Install" step.
        enum class Page
        {
            Welcome,
            Destination,
            Ref,
            Options,
            Review,
            Progress,
            Done
        };

        constexpr int kStepCount = 7;
        constexpr const char* kStepLabels[kStepCount] = {"Welcome", "Location", "Version", "Components",
                                                         "Review",  "Install",  "Done"};

        constexpr const char* kPresetNames[] = {"Defaults",       "All on",   "Minimal",
                                                "Linux-friendly", "Shipping", "Development"};
        constexpr int kPresetCount = static_cast<int>(sizeof(kPresetNames) / sizeof(kPresetNames[0]));

        constexpr const char* kRefLabels[] = {"Stable", "Nightly", "Working"};
        constexpr const char* kRefValues[] = {"stable", "nightly", "Working"};
        constexpr const char* kRefHints[] = {"The most recent release tag (a tag name; the clone fails if absent).",
                                             "The latest nightly tag.", "The latest development branch (default)."};
        constexpr int kRefChoiceCount = static_cast<int>(sizeof(kRefLabels) / sizeof(kRefLabels[0]));

        // Geometry in logical pixels at 100 %; every use goes through ui::Px.
        constexpr float kRailWidth = 220.0f;
        constexpr float kBandHeight = 84.0f;
        constexpr float kBandPad = 20.0f;
        constexpr float kLogomarkSize = 36.0f;
        constexpr float kStepRowHeight = 44.0f;
        constexpr float kStepMarkerRadius = 12.0f;
        constexpr float kFooterHeight = 64.0f;
        constexpr float kButtonWidth = 104.0f;
#ifdef _WIN32
        constexpr float kBrowseWidth = 104.0f;
#endif
        constexpr float kListPadX = 12.0f;
        constexpr float kListPadY = 8.0f;
        constexpr float kComfortableBodyPx = 16.0f;

        constexpr int kFailureTailLines = 20;
        // An exception escaping Installer::Run is reported as README exit code 4 (the install
        // location could not be used); the exception text is in the log.
        constexpr int kUnexpectedFailureExit = 4;

        constexpr ImGuiChildFlags kPanelFlags = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened;
        constexpr ImGuiWindowFlags kFixedFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        struct GuiState
        {
            Page page = Page::Welcome;
            char destBuf[1024] = {};
            char refBuf[256] = "Working";
            int presetIdx = 0; // Index into kPresetNames; -1 once a component was toggled by hand.
            bool focusPath = false;
            bool updateChosen = false;
            bool quit = false;
            bool closeWhenFinished = false;
            std::string priorPath;
            std::optional<InstallState> prior;

            // Install bookkeeping. The worker owns the context while started && !finished.
            bool started = false;
            std::thread worker;
            std::atomic<bool> finished{false};
            std::atomic<int> exitCode{0};

            std::mutex logMutex;
            std::vector<std::string> logLines;

            GuiState() = default;
            GuiState(const GuiState&) = delete;
            GuiState& operator=(const GuiState&) = delete;

            // The worker logs into this object, so it must end before any member is destroyed.
            ~GuiState()
            {
                if (worker.joinable())
                    worker.join();
            }

            bool Running() const { return started && !finished.load(); }

            void AppendLog(const std::string& line)
            {
                std::lock_guard<std::mutex> lock(logMutex);
                logLines.push_back(line);
            }
        };

        // Releases the platform backend on every exit path of Run().
        struct BackendSession
        {
            BackendSession() = default;
            BackendSession(const BackendSession&) = delete;
            BackendSession& operator=(const BackendSession&) = delete;
            ~BackendSession() { GuiBackend_Shutdown(); }
        };

        template <std::size_t N> void CopyToBuffer(char (&buffer)[N], const std::string& text)
        {
            std::snprintf(buffer, N, "%s", text.c_str());
        }

        std::string Trimmed(const char* text)
        {
            std::string value = text != nullptr ? text : "";
            const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
            while (!value.empty() && isSpace(value.back()))
                value.pop_back();
            std::size_t first = 0;
            while (first < value.size() && isSpace(value[first]))
                ++first;
            return value.substr(first);
        }

#ifdef _WIN32
        std::uint32_t PackRgb(ui::Rgba c)
        {
            const auto channel = [](float v) { return static_cast<std::uint32_t>(std::lround(v * 255.0f)) & 0xFFu; };
            return (channel(c.r) << 16) | (channel(c.g) << 8) | channel(c.b);
        }
#endif

        void ApplyPreset(InstallerContext& ctx, int idx)
        {
            ctx.configManager.InitDefaults();
            switch (idx)
            {
            case 0:
                ctx.configManager.ApplyPresetDefaults();
                break;
            case 1:
                ctx.configManager.ApplyPresetAllOn();
                break;
            case 2:
                ctx.configManager.ApplyPresetMinimal();
                break;
            case 3:
                ctx.configManager.ApplyPresetLinuxFriendly();
                break;
            case 4:
                ctx.configManager.ApplyPresetShipping();
                break;
            case 5:
                ctx.configManager.ApplyPresetDevelopment();
                break;
            default:
                ctx.configManager.ApplyPresetDefaults();
                break;
            }
        }

        std::size_t EnabledCount(const InstallerContext& ctx)
        {
            std::size_t enabled = 0;
            for (const auto& option : ctx.configManager.config.options)
            {
                if (option.currentValue)
                    ++enabled;
            }
            return enabled;
        }

        int RefChoiceIndex(const std::string& ref)
        {
            for (int i = 0; i < kRefChoiceCount; ++i)
            {
                if (ref == kRefValues[i])
                    return i;
            }
            return -1;
        }

        // Re-read the install record when the destination text changed. Changing the folder drops
        // an earlier "Update this install" choice, which belonged to the previous folder.
        void RefreshPrior(GuiState& s)
        {
            const std::string path = Trimmed(s.destBuf);
            if (path == s.priorPath)
                return;
            s.priorPath = path;
            s.prior.reset();
            s.updateChosen = false;
            if (path.empty() || !InstallState::Exists(path))
                return;
            InstallState loaded;
            if (InstallState::Load(path, loaded))
                s.prior = std::move(loaded);
        }

        // Update mode restores what the previous install used, as the TUI does: the ref, and every
        // option the record knows, on top of the defaults.
        void ChooseUpdate(GuiState& s, InstallerContext& ctx)
        {
            s.updateChosen = true;
            if (!s.prior->ref.empty())
                CopyToBuffer(s.refBuf, s.prior->ref);
            ctx.configManager.InitDefaults();
            for (auto& option : ctx.configManager.config.options)
            {
                const auto it = s.prior->options.find(option.cmakeVar);
                if (it != s.prior->options.end())
                    option.currentValue = it->second;
            }
            s.presetIdx = -1;
        }

        void StartInstall(GuiState& s, InstallerContext& ctx)
        {
            ctx.destination = Trimmed(s.destBuf);
            ctx.ref = Trimmed(s.refBuf);
            ctx.log = [&s](const std::string& line) { s.AppendLog(line); };
            s.started = true;
            s.finished = false;
            s.page = Page::Progress;
            try
            {
                s.worker = std::thread(
                    [&s, &ctx]()
                    {
                        int rc = kUnexpectedFailureExit;
                        try
                        {
                            rc = Installer::Run(ctx);
                        }
                        catch (const std::exception& error)
                        {
                            s.AppendLog(std::string("error: unexpected failure: ") + error.what());
                        }
                        catch (...)
                        {
                            s.AppendLog("error: unexpected failure");
                        }
                        s.exitCode = rc;
                        s.finished = true;
                    });
            }
            catch (const std::system_error& error)
            {
                s.AppendLog(std::string("error: could not start the install thread: ") + error.what());
                s.exitCode = kUnexpectedFailureExit;
                s.finished = true;
            }
        }

        // Join the worker once it reported completion and move to the result page.
        void CollectFinishedInstall(GuiState& s)
        {
            if (!s.started || !s.finished.load())
                return;
            if (s.worker.joinable())
                s.worker.join();
            if (s.page == Page::Progress)
                s.page = Page::Done;
        }

        const char* NextBlockedReason(const GuiState& s)
        {
            switch (s.page)
            {
            case Page::Destination:
                if (Trimmed(s.destBuf).empty())
                    return "Enter an install folder.";
                if (s.prior && !s.updateChosen)
                    return "This folder already holds an install. Choose Update, or pick another folder.";
                return nullptr;
            case Page::Ref:
                if (Trimmed(s.refBuf).empty())
                    return "Enter a branch or tag.";
                return nullptr;
            default:
                return nullptr;
            }
        }

        void Advance(GuiState& s, InstallerContext& ctx)
        {
            switch (s.page)
            {
            case Page::Welcome:
            case Page::Destination:
            case Page::Ref:
            case Page::Options:
                s.page = static_cast<Page>(static_cast<int>(s.page) + 1);
                break;
            case Page::Review:
                StartInstall(s, ctx);
                break;
            case Page::Progress:
                break;
            case Page::Done:
                s.quit = true;
                break;
            }
        }

        void Retreat(GuiState& s)
        {
            switch (s.page)
            {
            case Page::Destination:
            case Page::Ref:
            case Page::Options:
            case Page::Review:
                s.page = static_cast<Page>(static_cast<int>(s.page) - 1);
                break;
            case Page::Welcome:
            case Page::Progress:
            case Page::Done:
                break;
            }
        }

        // --- log helpers ---------------------------------------------------------------------

        std::string LatestLogLine(GuiState& s)
        {
            std::lock_guard<std::mutex> lock(s.logMutex);
            return s.logLines.empty() ? std::string() : s.logLines.back();
        }

        std::string JoinedLog(GuiState& s)
        {
            std::lock_guard<std::mutex> lock(s.logMutex);
            std::string joined;
            for (const std::string& line : s.logLines)
            {
                joined += line;
                joined += '\n';
            }
            return joined;
        }

        // A scrolling monospace view of the log lines from @p firstLine on; follows the end of the
        // log while the user has not scrolled away from it.
        void DrawLogView(GuiState& s, const char* id, std::size_t firstLine, bool followEnd)
        {
            ui::ScopedFont mono(ui::FontRole::MonoSmall);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px2(kListPadX, kListPadY));
            const bool open = ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                                                ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PopStyleVar();
            if (open)
            {
                const bool following = followEnd && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
                std::lock_guard<std::mutex> lock(s.logMutex);
                const std::size_t total = s.logLines.size();
                const std::size_t first = firstLine < total ? firstLine : total;
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(total - first));
                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        ImGui::TextUnformatted(s.logLines[first + static_cast<std::size_t>(i)].c_str());
                }
                if (following)
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }

        // --- step rail -----------------------------------------------------------------------

        enum class StepState
        {
            Done,
            Current,
            Pending,
            Failed
        };

        StepState StateOfStep(int step, const GuiState& s)
        {
            const int current = static_cast<int>(s.page);
            if (s.page == Page::Done)
            {
                const bool failed = s.exitCode.load() != 0;
                if (step == static_cast<int>(Page::Progress) && failed)
                    return StepState::Failed;
                if (step == static_cast<int>(Page::Done) && failed)
                    return StepState::Current;
                return StepState::Done;
            }
            if (step < current)
                return StepState::Done;
            return step == current ? StepState::Current : StepState::Pending;
        }

        const char* StepWord(StepState state)
        {
            switch (state)
            {
            case StepState::Done:
                return "Done";
            case StepState::Current:
                return "Current";
            case StepState::Failed:
                return "Failed";
            case StepState::Pending:
                break;
            }
            return nullptr;
        }

        ui::Role StepWordRole(StepState state)
        {
            switch (state)
            {
            case StepState::Done:
                return ui::Role::SuccessText;
            case StepState::Failed:
                return ui::Role::ErrorText;
            case StepState::Current:
            case StepState::Pending:
                break;
            }
            return ui::Role::AccentText;
        }

        // Done = check in a success disc, current = ember ring with the number, failed = cross in
        // an error disc, pending = grey ring with the number. The shape differs, not just the hue.
        void DrawStepMarker(ImDrawList* drawList, ImVec2 c, float r, StepState state, int number)
        {
            const float ring = ui::Px(2.0f);
            const float stroke = ui::Px(2.0f);
            switch (state)
            {
            case StepState::Done:
            {
                drawList->AddCircleFilled(c, r, ui::Col(ui::Role::SuccessFill));
                drawList->AddCircle(c, r, ui::Col(ui::Role::SuccessText), 0, ring);
                drawList->PathLineTo(ImVec2(c.x - r * 0.42f, c.y + r * 0.02f));
                drawList->PathLineTo(ImVec2(c.x - r * 0.10f, c.y + r * 0.36f));
                drawList->PathLineTo(ImVec2(c.x + r * 0.46f, c.y - r * 0.34f));
                drawList->PathStroke(ui::Col(ui::Role::SuccessText), ImDrawFlags_None, stroke);
                return;
            }
            case StepState::Failed:
            {
                drawList->AddCircleFilled(c, r, ui::Col(ui::Role::ErrorFill));
                drawList->AddCircle(c, r, ui::Col(ui::Role::ErrorText), 0, ring);
                const float arm = r * 0.34f;
                drawList->AddLine(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm),
                                  ui::Col(ui::Role::ErrorText), stroke);
                drawList->AddLine(ImVec2(c.x - arm, c.y + arm), ImVec2(c.x + arm, c.y - arm),
                                  ui::Col(ui::Role::ErrorText), stroke);
                return;
            }
            case StepState::Current:
                drawList->AddCircleFilled(c, r, ui::Col(ui::Role::SurfaceSelected));
                drawList->AddCircle(c, r, ui::Col(ui::Role::AccentFill), 0, ring);
                break;
            case StepState::Pending:
                drawList->AddCircle(c, r, ui::Col(ui::Role::BorderStrong), 0, ui::Px(1.0f));
                break;
            }
            char digits[8];
            std::snprintf(digits, sizeof(digits), "%d", number);
            ui::ScopedFont font(state == StepState::Current ? ui::FontRole::Strong : ui::FontRole::Small);
            const ImVec2 size = ImGui::CalcTextSize(digits);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f),
                              ui::Col(state == StepState::Current ? ui::Role::TextPrimary : ui::Role::TextSecondary),
                              digits);
        }

        // Brand band, then the seven steps. Returns the page a click asked to go back to, or -1.
        int DrawRail(const GuiState& s)
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetWindowPos();
            const float width = ImGui::GetWindowWidth();
            const float bandHeight = ui::Px(kBandHeight);
            const float padX = ImGui::GetStyle().WindowPadding.x;

            // The ink is chosen by measured contrast on the band; High Contrast gets a flat band.
            const ui::BrandBandStyle band = ui::ChooseBrandBandStyle(ui::ActiveTokens());
            const ImVec2 bandMax(origin.x + width, origin.y + bandHeight);
            if (band.gradient)
                ui::DrawBrandGradient(drawList, origin, bandMax);
            else
                drawList->AddRectFilled(origin, bandMax, ui::Col(ui::Role::SurfaceRaised));
            const float markSize = ui::Px(kLogomarkSize);
            const float markLeft = origin.x + ui::Px(kBandPad);
            ui::DrawLogomark(drawList, ImVec2(markLeft + markSize * 0.5f, origin.y + bandHeight * 0.5f), markSize,
                             false, ui::Col(band.ink));
            ImGui::SetCursorPos(ImVec2(ui::Px(kBandPad) + markSize + ui::Px(12.0f), ui::Px(kBandPad + 2.0f)));
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::ColV(band.ink));
            {
                ui::ScopedFont title(ui::FontRole::H2);
                ImGui::TextUnformatted("Spark Installer");
            }
            {
                ui::ScopedFont version(ui::FontRole::MonoSmall);
                ImGui::Text("v%s", kInstallerVersion);
            }
            ImGui::PopStyleColor();
            ImGui::EndGroup();

            ImGui::SetCursorPos(ImVec2(padX, bandHeight + ui::Px(kBandPad)));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
            const float rowHeight = ui::Px(kStepRowHeight);
            const float radius = ui::Px(kStepMarkerRadius);
            const float rowWidth = width - 2.0f * padX;
            const int current = static_cast<int>(s.page);
            const bool canNavigate = s.page != Page::Progress && s.page != Page::Done;
            int requested = -1;
            for (int i = 0; i < kStepCount; ++i)
            {
                const StepState state = StateOfStep(i, s);
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const bool clickable = canNavigate && i < current;
                ImGui::PushID(i);
                if (clickable)
                {
                    if (ImGui::InvisibleButton("##step", ImVec2(rowWidth, rowHeight)))
                        requested = i;
                }
                else
                {
                    ImGui::Dummy(ImVec2(rowWidth, rowHeight));
                }
                ImGui::PopID();
                if (clickable && ImGui::IsItemHovered())
                {
                    drawList->AddRectFilled(rowMin, ImVec2(rowMin.x + rowWidth, rowMin.y + rowHeight),
                                            ui::Col(ui::Role::SurfaceHover), ui::Px(ui::ActiveTokens().metrics.rFrame));
                }

                const ImVec2 center(rowMin.x + radius, rowMin.y + rowHeight * 0.5f);
                if (i + 1 < kStepCount)
                {
                    drawList->AddLine(ImVec2(center.x, center.y + radius),
                                      ImVec2(center.x, center.y + rowHeight - radius), ui::Col(ui::Role::BorderSubtle),
                                      ui::Px(1.0f));
                }
                DrawStepMarker(drawList, center, radius, state, i + 1);

                const char* word = StepWord(state);
                const ui::FontRole labelRole = state == StepState::Current ? ui::FontRole::Strong : ui::FontRole::Body;
                float labelHeight = 0.0f;
                float wordHeight = 0.0f;
                {
                    ui::ScopedFont font(labelRole);
                    labelHeight = ImGui::GetTextLineHeight();
                }
                if (word != nullptr)
                {
                    ui::ScopedFont font(ui::FontRole::Small);
                    wordHeight = ImGui::GetTextLineHeight();
                }
                const float textX = rowMin.x + 2.0f * radius + ui::Px(12.0f);
                const float textY = rowMin.y + (rowHeight - labelHeight - wordHeight) * 0.5f;
                {
                    ui::ScopedFont font(labelRole);
                    drawList->AddText(
                        ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, textY),
                        ui::Col(state == StepState::Pending ? ui::Role::TextSecondary : ui::Role::TextPrimary),
                        kStepLabels[i]);
                }
                if (word != nullptr)
                {
                    ui::ScopedFont font(ui::FontRole::Small);
                    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, textY + labelHeight),
                                      ui::Col(StepWordRole(state)), word);
                }
            }
            ImGui::PopStyleVar();
            return requested;
        }

        // --- pages ---------------------------------------------------------------------------

        void PageTitle(const char* title, const char* subtitle)
        {
            {
                ui::ScopedFont heading(ui::FontRole::H1);
                ImGui::TextUnformatted(title);
            }
            if (subtitle != nullptr)
                ui::SecondaryText("%s", subtitle);
            ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
        }

        void DrawWelcome()
        {
            PageTitle("Set up Spark Engine", "Build the engine from source on this machine.");
            ImGui::TextWrapped("This tool will clone the SparkEngine repository (with submodules), "
                               "let you pick which engine modules to build, and configure + build "
                               "the engine on your machine.");
            ImGui::Dummy(ImVec2(0.0f, ui::Px(8.0f)));
            ui::SubHeader("What happens next");
            ImGui::Bullet();
            ImGui::TextWrapped("Choose an install folder and the version to fetch.");
            ImGui::Bullet();
            ImGui::TextWrapped("Choose which engine components to build.");
            ImGui::Bullet();
            ImGui::TextWrapped("Review your choices, then install. Nothing is changed before that.");
            ImGui::Dummy(ImVec2(0.0f, ui::Px(8.0f)));
            ui::Banner(ui::Status::Info, "The installer checks your machine before it changes anything.");
        }

        void DrawDestination(GuiState& s, InstallerContext& ctx)
        {
            PageTitle("Where should Spark Engine go?",
                      "A new or empty folder works best. The installer creates it if it does not exist.");
            ui::SecondaryText("Install folder");
#ifdef _WIN32
            const float browseWidth = ui::Px(kBrowseWidth);
            ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
#else
            ImGui::SetNextItemWidth(-FLT_MIN);
#endif
            if (s.focusPath)
            {
                ImGui::SetKeyboardFocusHere();
                s.focusPath = false;
            }
            ImGui::InputText("##dest", s.destBuf, sizeof(s.destBuf));
#ifdef _WIN32
            ImGui::SameLine();
            if (ui::SecondaryButton("Browse...", ImVec2(browseWidth, 0.0f)))
            {
                std::string chosen;
                if (GuiBackend_PickFolder(Trimmed(s.destBuf), chosen))
                    CopyToBuffer(s.destBuf, chosen);
            }
#endif
            RefreshPrior(s);

            if (!s.prior)
                return;
            ImGui::Dummy(ImVec2(0.0f, ui::Px(8.0f)));
            ui::Banner(ui::Status::Info, "Spark Engine is already installed in this folder.");
            ui::KeyValue("Version", "%s", s.prior->ref.empty() ? "(unknown)" : s.prior->ref.c_str());
            ui::KeyValue("Commit", "%s", s.prior->commit.empty() ? "(unknown)" : s.prior->commit.c_str());
            ui::KeyValue("Built", "%s", s.prior->builtAt.empty() ? "(unknown)" : s.prior->builtAt.c_str());
            ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
            if (s.updateChosen)
            {
                ui::StatusText(ui::Status::Success, "Update selected. Your previous options are restored.");
                return;
            }
            if (ui::PrimaryButton("Update this install (keeps your options)"))
                ChooseUpdate(s, ctx);
            ImGui::SameLine();
            if (ui::SecondaryButton("Choose another folder"))
                s.focusPath = true;
        }

        void DrawVersion(GuiState& s)
        {
            PageTitle("Which version?", "Pick a channel, or type any branch or tag.");
            int choice = RefChoiceIndex(Trimmed(s.refBuf));
            if (ui::SegmentedControl("##ref", kRefLabels, kRefChoiceCount, &choice))
                CopyToBuffer(s.refBuf, kRefValues[choice]);
            ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
            ImGui::PushTextWrapPos(0.0f);
            for (int i = 0; i < kRefChoiceCount; ++i)
            {
                ui::TextRole(i == choice ? ui::Role::TextPrimary : ui::Role::TextSecondary, "%s  %s", kRefLabels[i],
                             kRefHints[i]);
            }
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0.0f, ui::Px(8.0f)));
            ui::SecondaryText("Branch or tag");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##ref_custom", s.refBuf, sizeof(s.refBuf));
        }

        void DrawComponents(GuiState& s, InstallerContext& ctx)
        {
            PageTitle("Choose components", "Pick a preset, then switch single components on or off.");
            if (ui::SegmentedControl("##preset", kPresetNames, kPresetCount, &s.presetIdx))
                ApplyPreset(ctx, s.presetIdx);
            ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
            ui::KeyValue("Enabled", "%zu of %zu", EnabledCount(ctx), ctx.configManager.config.options.size());
            ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px2(kListPadX, kListPadY));
            const bool open = ImGui::BeginChild("##components", ImVec2(0.0f, 0.0f),
                                                ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
            ImGui::PopStyleVar();
            if (open)
            {
                constexpr int kCategoryCount = static_cast<int>(SparkBuild::OptionCategory::Experimental) + 1;
                for (int c = 0; c < kCategoryCount; ++c)
                {
                    const auto category = static_cast<SparkBuild::OptionCategory>(c);
                    bool any = false;
                    for (const auto& option : ctx.configManager.config.options)
                        any = any || option.category == category;
                    if (!any)
                        continue;
                    ui::SubHeader(SparkBuild::CategoryDisplayName(category));
                    ImGui::PushID(c);
                    if (ui::BeginPropertyGrid("##grid"))
                    {
                        for (auto& option : ctx.configManager.config.options)
                        {
                            if (option.category != category)
                                continue;
                            ImGui::PushID(option.cmakeVar.c_str());
                            ui::PropertyLabel(option.displayName.c_str(), option.description.c_str());
                            if (ImGui::Checkbox("##enabled", &option.currentValue))
                                s.presetIdx = -1;
                            ImGui::PopID();
                        }
                        ui::EndPropertyGrid();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }

        void DrawReview(GuiState& s, InstallerContext& ctx)
        {
            PageTitle("Ready to install", "Nothing is changed until you select Install.");
            const std::string destination = Trimmed(s.destBuf);
            ui::SecondaryText("Install folder");
            {
                ui::ScopedFont mono(ui::FontRole::Mono);
                ImGui::TextWrapped("%s", destination.c_str());
            }
            ui::KeyValue("Version", "%s", Trimmed(s.refBuf).c_str());
            ui::KeyValue("Repository", "%s", ctx.repoUrl.c_str());
            ui::KeyValue("Action", "%s", s.updateChosen ? "Update this install" : "Install");
            ui::KeyValue("Preset", "%s", s.presetIdx >= 0 ? kPresetNames[s.presetIdx] : "Custom");
            ui::KeyValue("Components", "%zu of %zu enabled", EnabledCount(ctx),
                         ctx.configManager.config.options.size());
            ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
            ui::SubHeader("Enabled components");
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px2(kListPadX, kListPadY));
            const bool open = ImGui::BeginChild("##enabled", ImVec2(0.0f, 0.0f),
                                                ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
            ImGui::PopStyleVar();
            if (open)
            {
                for (const auto& option : ctx.configManager.config.options)
                {
                    if (option.currentValue)
                        ImGui::BulletText("%s", option.displayName.c_str());
                }
            }
            ImGui::EndChild();
        }

        void DrawProgress(GuiState& s)
        {
            PageTitle("Installing Spark Engine", "This can take a while. Keep this window open.");
            ui::ProgressIndeterminate("##progress", ImVec2(0.0f, ui::Px(6.0f)));
            ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
            const std::string latest = LatestLogLine(s);
            ImGui::PushTextWrapPos(0.0f);
            ui::MonoText("%s", latest.empty() ? "Starting..." : latest.c_str());
            ImGui::PopTextWrapPos();
            if (s.closeWhenFinished)
            {
                ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
                ui::Banner(ui::Status::Info,
                           "Installation is running. Closing will wait for the current step to finish.");
            }
            ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
            if (ui::SectionHeader("Full log", nullptr, false))
            {
                if (ui::SecondaryButton("Copy log"))
                    ImGui::SetClipboardText(JoinedLog(s).c_str());
                DrawLogView(s, "##log", 0, true);
            }
        }

        void DrawDone(GuiState& s, InstallerContext& ctx)
        {
            const int rc = s.exitCode.load();
            if (rc == 0)
            {
                PageTitle("Spark Engine is ready", nullptr);
                ui::Banner(ui::Status::Success,
                           ctx.skipBuild ? "Engine source installed (build skipped)." : "Engine built successfully.");
                ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
                ui::KeyValue("Location", "%s", ctx.destination.c_str());
                ui::KeyValue("Version", "%s", ctx.ref.c_str());
                if (!ctx.skipBuild && !ctx.configManager.config.buildPath.empty())
                    ui::KeyValue("Build folder", "%s", ctx.configManager.config.buildPath.c_str());
                return;
            }

            PageTitle("The install did not finish", nullptr);
            char message[160];
            std::snprintf(message, sizeof(message),
                          "Install failed (exit code %d). The installer README lists what each code means.", rc);
            ui::Banner(ui::Status::Error, message);
            ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
            ui::SubHeader("Last log lines");
            std::size_t first = 0;
            {
                std::lock_guard<std::mutex> lock(s.logMutex);
                const std::size_t total = s.logLines.size();
                first = total > static_cast<std::size_t>(kFailureTailLines)
                            ? total - static_cast<std::size_t>(kFailureTailLines)
                            : 0;
            }
            if (ui::SecondaryButton("Copy log"))
                ImGui::SetClipboardText(JoinedLog(s).c_str());
            DrawLogView(s, "##tail", first, false);
        }

        void DrawPage(GuiState& s, InstallerContext& ctx)
        {
            switch (s.page)
            {
            case Page::Welcome:
                DrawWelcome();
                break;
            case Page::Destination:
                DrawDestination(s, ctx);
                break;
            case Page::Ref:
                DrawVersion(s);
                break;
            case Page::Options:
                DrawComponents(s, ctx);
                break;
            case Page::Review:
                DrawReview(s, ctx);
                break;
            case Page::Progress:
                DrawProgress(s);
                break;
            case Page::Done:
                DrawDone(s, ctx);
                break;
            }
        }

        // --- footer and frame ----------------------------------------------------------------

        struct FooterAction
        {
            bool cancel = false;
            bool back = false;
            bool primary = false;
        };

        FooterAction DrawFooter(const GuiState& s)
        {
            FooterAction action;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetWindowPos();
            drawList->AddLine(origin, ImVec2(origin.x + ImGui::GetWindowWidth(), origin.y),
                              ui::Col(ui::Role::BorderSubtle), ui::Px(1.0f));

            const ImVec2 buttonSize(ui::Px(kButtonWidth), 0.0f);
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float padX = ImGui::GetStyle().WindowPadding.x;
            const float y = (ImGui::GetWindowHeight() - ImGui::GetFrameHeight()) * 0.5f;
            const float primaryX = ImGui::GetWindowWidth() - padX - buttonSize.x;

            if (s.page != Page::Done)
            {
                ImGui::SetCursorPos(ImVec2(padX, y));
                ui::ScopedDisabled disabled(s.Running(), "A started installation cannot be cancelled.");
                action.cancel = ui::SecondaryButton("Cancel", buttonSize);
            }

            const bool showBack =
                s.page == Page::Destination || s.page == Page::Ref || s.page == Page::Options || s.page == Page::Review;
            if (showBack)
            {
                ImGui::SetCursorPos(ImVec2(primaryX - spacing - buttonSize.x, y));
                action.back = ui::SecondaryButton("Back", buttonSize);
            }

            const char* label = nullptr;
            switch (s.page)
            {
            case Page::Welcome:
            case Page::Destination:
            case Page::Ref:
            case Page::Options:
                label = "Next";
                break;
            case Page::Review:
                label = "Install";
                break;
            case Page::Done:
                label = "Close";
                break;
            case Page::Progress:
                break;
            }
            if (label != nullptr)
            {
                const char* blocked = NextBlockedReason(s);
                ImGui::SetCursorPos(ImVec2(primaryX, y));
                ui::ScopedDisabled disabled(blocked != nullptr, blocked);
                action.primary = ui::PrimaryButton(label, buttonSize);
            }
            return action;
        }

        void DrawWizard(GuiState& s, InstallerContext& ctx)
        {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::Begin("SparkInstaller", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | kFixedFlags);
            ImGui::PopStyleVar();

            int railRequest = -1;
            {
                ui::ScopedRoleColor railBackground(ImGuiCol_ChildBg, ui::Role::SurfaceSunken);
                ImGui::BeginChild("##rail", ImVec2(ui::Px(kRailWidth), 0.0f), kPanelFlags, kFixedFlags);
                railRequest = DrawRail(s);
                ImGui::EndChild();
            }
            ImGui::SameLine(0.0f, 0.0f);

            FooterAction footer;
            ImGui::BeginChild("##column", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened, kFixedFlags);
            const float footerHeight = ui::Px(kFooterHeight);
            ImGui::BeginChild("##content", ImVec2(0.0f, -(footerHeight + ImGui::GetStyle().ItemSpacing.y)),
                              kPanelFlags);
            DrawPage(s, ctx);
            ImGui::EndChild();
            ImGui::BeginChild("##footer", ImVec2(0.0f, footerHeight), kPanelFlags, kFixedFlags);
            footer = DrawFooter(s);
            ImGui::EndChild();
            ImGui::EndChild();
            ImGui::End();

            if (railRequest >= 0)
                s.page = static_cast<Page>(railRequest);
            if (footer.cancel)
                s.quit = true;
            if (footer.back)
                Retreat(s);
            if (footer.primary)
                Advance(s, ctx);
        }

        // Tokens for the installer: Ember Dark (or High Contrast when the desktop uses a
        // high-contrast theme) with the Comfortable metrics and body size of the spec.
        ui::Tokens MakeTokens(bool highContrast)
        {
            const ui::Appearance appearance = highContrast ? ui::Appearance::HighContrast : ui::Appearance::Dark;
            ui::Tokens tokens = ui::GenerateTokens(ui::BuiltinSeed(ui::PaletteId::Ember), appearance);
            const float focusRing = tokens.metrics.focusRing;
            tokens.metrics = ui::MetricsFor(ui::Density::Standard, ui::MetricsProfile::Comfortable);
            tokens.metrics.focusRing = focusRing;
            tokens.type.body = kComfortableBodyPx;
            tokens.type.strong = kComfortableBodyPx;
            return tokens;
        }
    } // namespace

    bool WizardGui::Run(InstallerContext& ctx)
    {
        m_exitCode = 0;
        if (!GuiBackend_Init(kGuiWindowWidth, kGuiWindowHeight, "SparkInstaller"))
            return false;
        BackendSession backend;

        const GuiSystemPreferences preferences = GuiBackend_QuerySystemPreferences();
        const ui::Tokens tokens = MakeTokens(preferences.highContrast);
        const fs::path fontDirectory = ui::FindFontDirectory(GuiBackend_ExecutableDir());
        ImGuiIO& io = ImGui::GetIO();
        const ui::FontSet fonts = ui::LoadFontSet(*io.Fonts, fontDirectory, tokens.type.body);
        if (fonts.body != nullptr)
            io.FontDefault = fonts.body;
        ui::Install(ImGui::GetCurrentContext(), tokens, GuiBackend_DpiScale(), 1.0f, preferences.reduceMotion);
        ui::InstallFocusRing(ImGui::GetCurrentContext());
#ifdef _WIN32
        GuiBackend_SetCaptionColors(PackRgb(ui::Get(tokens, ui::Role::SurfaceSunken)),
                                    PackRgb(ui::Get(tokens, ui::Role::TextPrimary)));
#endif

        GuiState s;
        if (!fonts.diagnostic.empty())
            s.AppendLog("UI fonts: " + fonts.diagnostic);

        std::string initialDest = ctx.destination;
        if (initialDest.empty())
        {
            std::error_code ec;
            const fs::path cwd = fs::current_path(ec);
            initialDest = ec ? std::string("SparkEngine") : (cwd / "SparkEngine").string();
        }
        CopyToBuffer(s.destBuf, initialDest);
        if (!ctx.ref.empty())
            CopyToBuffer(s.refBuf, ctx.ref);
        ApplyPreset(ctx, s.presetIdx);

        const LogSink savedLog = ctx.log;
        while (GuiBackend_BeginFrame())
        {
            if (GuiBackend_ConsumeCloseRequest())
            {
                // There is no cancel for a started install: closing waits for it to finish.
                if (s.Running())
                    s.closeWhenFinished = true;
                else
                    s.quit = true;
            }
            CollectFinishedInstall(s);
            if (s.closeWhenFinished && !s.Running())
                s.quit = true;

            DrawWizard(s, ctx);
            GuiBackend_EndFrame();
            if (s.quit)
                break;
        }

        if (s.worker.joinable())
            s.worker.join();
        ctx.log = savedLog;
        m_exitCode = s.started ? s.exitCode.load() : 0;
        return true;
    }
} // namespace SparkInstaller
