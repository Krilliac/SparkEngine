#include "Installer.h"
#include "InstallerContext.h"
#include "Platform.h"
#include "tui/WizardTui.h"

#ifdef SPARKINSTALLER_ENABLE_GUI
#include "gui/WizardGui.h"
#endif

#include <cstring>
#include <iostream>
#include <string>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{
    void PrintHelp()
    {
        std::cout << "SparkInstaller " << SparkInstaller::kInstallerVersion << " — bootstrap SparkEngine\n\n";
        std::cout << "Usage:\n";
        std::cout << "  sparkinstaller                      Interactive TUI\n";
        std::cout << "  sparkinstaller --gui                Interactive GUI wizard\n";
        std::cout << "  sparkinstaller --headless --dest <dir> [--ref <branch|tag>]\n";
        std::cout << "                                      Non-interactive: accept flags, fail on missing input\n";
        std::cout << "  sparkinstaller --help               Show this help\n";
        std::cout << "  sparkinstaller --version            Show version\n\n";
        std::cout << "Flags:\n";
        std::cout << "  --dest <dir>        Install destination (default: ./SparkEngine)\n";
        std::cout << "  --ref <name>        Git branch or tag to clone (default: Working)\n";
        std::cout << "  --repo <url>        Override repo URL\n";
        std::cout << "  --skip-build        Clone only, do not configure/build\n";
        std::cout << "  --skip-submodules   Do not init/update submodules in Update mode\n";
        std::cout << "\nPlatform: " SPARK_PLATFORM_NAME "\n";
    }
} // namespace

int main(int argc, char* argv[])
{
#ifdef SPARK_PLATFORM_WINDOWS
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    SparkInstaller::InstallerContext ctx;
    ctx.log = [](const std::string& line) { std::cout << line << "\n"; };

    bool wantGui = false;
    bool headless = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: " << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h")
        {
            PrintHelp();
            return 0;
        }
        if (arg == "--version" || arg == "-v")
        {
            std::cout << "SparkInstaller " << SparkInstaller::kInstallerVersion << " (" SPARK_PLATFORM_NAME ")\n";
            return 0;
        }
        if (arg == "--gui")
            wantGui = true;
        else if (arg == "--headless")
            headless = true;
        else if (arg == "--dest")
            ctx.destination = next("--dest");
        else if (arg == "--ref")
            ctx.ref = next("--ref");
        else if (arg == "--repo")
            ctx.repoUrl = next("--repo");
        else if (arg == "--skip-build")
            ctx.skipBuild = true;
        else if (arg == "--skip-submodules")
            ctx.skipSubmoduleUpdate = true;
        else
        {
            std::cerr << "error: unknown argument: " << arg << "\n";
            PrintHelp();
            return 2;
        }
    }

    if (wantGui && headless)
    {
        std::cerr << "error: --gui and --headless are mutually exclusive\n";
        return 2;
    }

    if (headless)
    {
        ctx.frontend = SparkInstaller::Frontend::Headless;
        if (ctx.destination.empty())
        {
            std::cerr << "error: --headless requires --dest\n";
            return 2;
        }
        ctx.configManager.InitDefaults();
        return SparkInstaller::Installer::Run(ctx);
    }

    if (wantGui)
    {
#ifdef SPARKINSTALLER_ENABLE_GUI
        ctx.frontend = SparkInstaller::Frontend::Gui;
        if (!SparkInstaller::WizardGui::Run(ctx))
            return 0;
        return SparkInstaller::Installer::Run(ctx);
#else
        std::cerr << "error: this build was compiled without GUI support. Rerun without --gui.\n";
        return 2;
#endif
    }

    ctx.frontend = SparkInstaller::Frontend::Tui;
    if (!SparkInstaller::WizardTui::Run(ctx))
        return 0;
    return SparkInstaller::Installer::Run(ctx);
}
