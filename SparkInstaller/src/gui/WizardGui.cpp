#include "WizardGui.h"

#include "InstallState.h"
#include "Installer.h"

#include <imgui.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace SparkInstaller
{
    namespace fs = std::filesystem;

    namespace
    {
        enum class Page
        {
            Welcome,
            Destination,
            Ref,
            Options,
            Progress,
            Done
        };

        struct GuiState
        {
            Page page = Page::Welcome;
            char destBuf[1024] = {};
            char refBuf[256] = "Working";
            int presetIdx = 0; // 0 Defaults, 1 AllOn, 2 Minimal, 3 LinuxFriendly, 4 Shipping, 5 Development

            std::mutex logMutex;
            std::vector<std::string> logLines;
            std::atomic<bool> running{false};
            std::atomic<bool> finished{false};
            std::atomic<int> exitCode{0};

            void AppendLog(const std::string& s)
            {
                std::lock_guard<std::mutex> lock(logMutex);
                logLines.push_back(s);
            }
        };

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

        const char* kPresetNames[] = {"Defaults", "All on", "Minimal", "Linux-friendly", "Shipping", "Development"};
    } // namespace

    bool WizardGui::Run(InstallerContext& ctx)
    {
        if (!GuiBackend_Init(800, 600, "SparkInstaller"))
            return false;

        GuiState s;
        std::string initialDest =
            ctx.destination.empty() ? (fs::current_path() / "SparkEngine").string() : ctx.destination;
        std::snprintf(s.destBuf, sizeof(s.destBuf), "%s", initialDest.c_str());
        if (!ctx.ref.empty())
            std::snprintf(s.refBuf, sizeof(s.refBuf), "%s", ctx.ref.c_str());

        bool userAccepted = false;
        std::thread worker;

        while (GuiBackend_BeginFrame())
        {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("SparkInstaller", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

            switch (s.page)
            {
            case Page::Welcome:
                ImGui::TextUnformatted("SparkInstaller");
                ImGui::Separator();
                ImGui::TextWrapped("This tool will clone the SparkEngine repository (with submodules), "
                                   "let you pick which engine modules to build, and configure + build "
                                   "the engine on your machine.");
                ImGui::Dummy({0, 16});
                if (ImGui::Button("Next", {120, 0}))
                    s.page = Page::Destination;
                break;

            case Page::Destination:
                ImGui::TextUnformatted("Destination directory");
                ImGui::Separator();
                ImGui::InputText("##dest", s.destBuf, sizeof(s.destBuf));
                ImGui::Dummy({0, 16});
                if (ImGui::Button("Back", {80, 0}))
                    s.page = Page::Welcome;
                ImGui::SameLine();
                if (ImGui::Button("Next", {80, 0}))
                {
                    ctx.destination = s.destBuf;
                    if (InstallState::Exists(ctx.destination))
                    {
                        InstallState prior;
                        if (InstallState::Load(ctx.destination, prior))
                        {
                            std::snprintf(s.refBuf, sizeof(s.refBuf), "%s",
                                          prior.ref.empty() ? "Working" : prior.ref.c_str());
                        }
                    }
                    s.page = Page::Ref;
                }
                break;

            case Page::Ref:
                ImGui::TextUnformatted("Which version to fetch?");
                ImGui::Separator();
                ImGui::InputText("Branch or tag", s.refBuf, sizeof(s.refBuf));
                ImGui::Dummy({0, 4});
                if (ImGui::Button("Working"))
                    std::snprintf(s.refBuf, sizeof(s.refBuf), "Working");
                ImGui::SameLine();
                if (ImGui::Button("nightly"))
                    std::snprintf(s.refBuf, sizeof(s.refBuf), "nightly");
                ImGui::SameLine();
                if (ImGui::Button("stable"))
                    std::snprintf(s.refBuf, sizeof(s.refBuf), "stable");
                ImGui::Dummy({0, 16});
                if (ImGui::Button("Back", {80, 0}))
                    s.page = Page::Destination;
                ImGui::SameLine();
                if (ImGui::Button("Next", {80, 0}))
                {
                    ctx.ref = s.refBuf;
                    s.page = Page::Options;
                }
                break;

            case Page::Options:
                ImGui::TextUnformatted("Build preset");
                ImGui::Separator();
                ImGui::Combo("Preset", &s.presetIdx, kPresetNames, IM_ARRAYSIZE(kPresetNames));
                ImGui::Dummy({0, 16});
                if (ImGui::Button("Back", {80, 0}))
                    s.page = Page::Ref;
                ImGui::SameLine();
                if (ImGui::Button("Install", {100, 0}))
                {
                    ApplyPreset(ctx, s.presetIdx);
                    ctx.log = [&s](const std::string& line) { s.AppendLog(line); };
                    s.running = true;
                    s.finished = false;
                    worker = std::thread(
                        [&s, &ctx]()
                        {
                            int rc = Installer::Run(ctx);
                            s.exitCode = rc;
                            s.finished = true;
                            s.running = false;
                        });
                    s.page = Page::Progress;
                }
                break;

            case Page::Progress:
            {
                ImGui::TextUnformatted(s.finished ? (s.exitCode == 0 ? "Complete" : "Failed") : "Installing...");
                ImGui::Separator();
                ImGui::BeginChild("log", {0, -40}, true, ImGuiWindowFlags_HorizontalScrollbar);
                {
                    std::lock_guard<std::mutex> lock(s.logMutex);
                    for (const auto& line : s.logLines)
                        ImGui::TextUnformatted(line.c_str());
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
                        ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();
                if (s.finished)
                {
                    if (ImGui::Button("Done", {100, 0}))
                    {
                        userAccepted = (s.exitCode == 0);
                        s.page = Page::Done;
                    }
                }
                break;
            }

            case Page::Done:
                ImGui::TextUnformatted(s.exitCode == 0 ? "Engine built successfully." : "Install failed.");
                ImGui::Separator();
                if (ImGui::Button("Close", {100, 0}))
                {
                    ImGui::End();
                    GuiBackend_EndFrame();
                    goto shutdown;
                }
                break;
            }

            ImGui::End();
            GuiBackend_EndFrame();
        }

    shutdown:
        if (worker.joinable())
            worker.join();
        GuiBackend_Shutdown();
        // The GUI already invoked Installer::Run itself; main() does not need to run it again.
        // To keep the contract (main runs Installer::Run if Wizard returns true), the GUI returns
        // false so main does not double-run.
        (void)userAccepted;
        return false;
    }
} // namespace SparkInstaller
