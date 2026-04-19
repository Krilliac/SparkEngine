#include "WizardTui.h"

#include "InstallState.h"
#include "Terminal.h"

#include <filesystem>
#include <iostream>

namespace SparkInstaller
{
    namespace fs = std::filesystem;
    using SparkBuild::Term::Bold;
    using SparkBuild::Term::Cyan;
    using SparkBuild::Term::Dim;
    using SparkBuild::Term::Green;
    using SparkBuild::Term::Yellow;

    namespace
    {
        void PrintWelcome()
        {
            SparkBuild::Term::ClearScreen();
            SparkBuild::Term::PrintHeader("SparkInstaller — Bootstrap the SparkEngine");
            std::cout << "\n";
            std::cout << "This tool will:\n";
            std::cout << "  " << Green("1.") << " Clone Krilliac/SparkEngine (+ submodules)\n";
            std::cout << "  " << Green("2.") << " Let you pick which engine modules to build\n";
            std::cout << "  " << Green("3.") << " Configure and build the engine on your machine\n";
            std::cout << "\n";
            std::cout << Dim("Press Ctrl+C at any time to cancel.") << "\n\n";
        }

        std::string PromptDestination(const std::string& suggested)
        {
            std::cout << Bold("Destination directory") << " " << Dim("[" + suggested + "]") << ": ";
            std::string line;
            if (!std::getline(std::cin, line))
                return suggested;
            if (line.empty())
                return suggested;
            return line;
        }

        std::string PromptRef()
        {
            std::cout << "\n" << Bold("Which version to fetch?") << "\n";
            std::cout << "  " << Green("1.") << " Working        " << Dim("(latest development)") << "\n";
            std::cout << "  " << Green("2.") << " nightly        " << Dim("(latest nightly tag)") << "\n";
            std::cout << "  " << Green("3.") << " latest stable  " << Dim("(most recent release tag)") << "\n";
            std::cout << "  " << Green("4.") << " custom         " << Dim("(enter a branch/tag name)") << "\n";
            int choice = SparkBuild::Term::ReadInt("> ", 1, 4);
            switch (choice)
            {
            case 1:
                return "Working";
            case 2:
                return "nightly";
            case 3:
                return "stable"; // resolved as a tag name; git clone --branch will fail if absent
            case 4:
                return SparkBuild::Term::ReadLine("Branch or tag: ");
            default:
                return "Working";
            }
        }

        void ShowOptionsSummary(const SparkBuild::ConfigManager& cm)
        {
            std::cout << "\n"
                      << Bold("Build options") << " — " << cm.config.options.size()
                      << " toggles available, current preset shown:\n";
            size_t onCount = 0;
            for (const auto& o : cm.config.options)
                if (o.currentValue)
                    ++onCount;
            std::cout << "  " << Green(std::to_string(onCount)) << " enabled, "
                      << Yellow(std::to_string(cm.config.options.size() - onCount)) << " disabled\n";
        }

        bool PromptOptions(InstallerContext& ctx)
        {
            std::cout << "\n" << Bold("Preset:") << "\n";
            std::cout << "  " << Green("1.") << " Defaults       " << Dim("(recommended)") << "\n";
            std::cout << "  " << Green("2.") << " All on         " << Dim("(everything enabled)") << "\n";
            std::cout << "  " << Green("3.") << " Minimal        " << Dim("(tiny build)") << "\n";
            std::cout << "  " << Green("4.") << " Linux-friendly\n";
            std::cout << "  " << Green("5.") << " Shipping\n";
            std::cout << "  " << Green("6.") << " Development\n";
            int choice = SparkBuild::Term::ReadInt("> ", 1, 6);
            switch (choice)
            {
            case 1:
                ctx.configManager.ApplyPresetDefaults();
                break;
            case 2:
                ctx.configManager.ApplyPresetAllOn();
                break;
            case 3:
                ctx.configManager.ApplyPresetMinimal();
                break;
            case 4:
                ctx.configManager.ApplyPresetLinuxFriendly();
                break;
            case 5:
                ctx.configManager.ApplyPresetShipping();
                break;
            case 6:
                ctx.configManager.ApplyPresetDevelopment();
                break;
            default:
                ctx.configManager.ApplyPresetDefaults();
                break;
            }
            ShowOptionsSummary(ctx.configManager);
            return true;
        }
    } // namespace

    bool WizardTui::Run(InstallerContext& ctx)
    {
        SparkBuild::Term::EnableColors();
        PrintWelcome();

        std::string suggestedDest =
            ctx.destination.empty() ? (fs::current_path() / "SparkEngine").string() : ctx.destination;
        ctx.destination = PromptDestination(suggestedDest);

        // If the destination already looks like an install, offer update flow and reuse prior state.
        if (InstallState::Exists(ctx.destination))
        {
            InstallState prior;
            if (InstallState::Load(ctx.destination, prior))
            {
                std::cout << "\n" << Cyan("Existing install detected:") << "\n";
                std::cout << "  ref:    " << prior.ref << "\n";
                std::cout << "  commit: " << prior.commit << "\n";
                std::cout << "  built:  " << prior.builtAt << "\n";
                if (!SparkBuild::Term::ReadYesNo("\nUpdate this install?", true))
                    return false;
                ctx.ref = prior.ref.empty() ? ctx.ref : prior.ref;
                // Reapply stored options on top of defaults
                ctx.configManager.InitDefaults();
                for (auto& opt : ctx.configManager.config.options)
                {
                    auto it = prior.options.find(opt.cmakeVar);
                    if (it != prior.options.end())
                        opt.currentValue = it->second;
                }
                return true;
            }
        }

        ctx.ref = PromptRef();
        ctx.configManager.InitDefaults();
        PromptOptions(ctx);

        std::cout << "\n" << Bold("Ready to install:") << "\n";
        std::cout << "  Destination: " << ctx.destination << "\n";
        std::cout << "  Ref:         " << ctx.ref << "\n";
        std::cout << "  Repo:        " << ctx.repoUrl << "\n\n";
        return SparkBuild::Term::ReadYesNo("Proceed?", true);
    }
} // namespace SparkInstaller
