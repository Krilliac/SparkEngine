/**
 * @file main.cpp
 * @brief SparkLauncher entry point — Unity-Hub-style project picker that spawns SparkEditor.
 * @author Spark Engine Team
 * @date 2025
 */

#include "LauncherApp.h"
#include "LauncherBackend.h"
#include "LauncherPaths.h"
#include "LauncherProcess.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace
{
#ifndef SPARK_LAUNCHER_VERSION
#error "SPARK_LAUNCHER_VERSION must be supplied by the build system"
#endif
    constexpr const char* kVersion = "SparkLauncher " SPARK_LAUNCHER_VERSION;
} // namespace

namespace
{
    int ValidateLaunchRequest(const std::filesystem::path& projectFile,
                              const std::filesystem::path& binaryDirectory = {})
    {
        const auto resolvedBinaryDirectory =
            binaryDirectory.empty() ? std::filesystem::path(SparkLauncher::GetLauncherExecutablePath()).parent_path()
                                    : binaryDirectory;
        const auto request = SparkLauncher::BuildLaunchRequest(resolvedBinaryDirectory, projectFile,
                                                               SparkLauncher::LaunchTarget::Editor);
        if (!request)
        {
            std::cerr << "Launch request rejected: " << request.error() << '\n';
            return 2;
        }

        std::cout << "Launch request validated: target="
                  << SparkLauncher::LaunchTargetName(SparkLauncher::LaunchTarget::Editor)
                  << " executable=" << SparkLauncher::PathToUtf8(request->executable) << '\n';
        return 0;
    }
} // namespace

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR cmdLine, _In_ int)
try
{
    (void)cmdLine;
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    for (int index = 1; arguments && index < argumentCount; ++index)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--validate-launch-request")
        {
            if (index + 1 >= argumentCount)
            {
                std::cerr << "--validate-launch-request requires a project path\n";
                LocalFree(arguments);
                return 2;
            }
            const auto projectFile = std::filesystem::path(arguments[++index]);
            std::filesystem::path binaryDirectory;
            if (index + 1 < argumentCount)
            {
                if (std::wstring_view(arguments[index + 1]) != L"--binary-directory" || index + 2 >= argumentCount ||
                    index + 3 != argumentCount)
                {
                    std::cerr << "Unexpected argument after --validate-launch-request\n";
                    LocalFree(arguments);
                    return 2;
                }
                binaryDirectory = std::filesystem::path(arguments[index += 2]);
            }
            const int result = ValidateLaunchRequest(projectFile, binaryDirectory);
            LocalFree(arguments);
            return result;
        }
        if (argument == L"--version" || argument == L"-v")
        {
            std::cout << kVersion << '\n';
            LocalFree(arguments);
            return 0;
        }
        if (argument == L"--help" || argument == L"-h")
        {
            std::cout << kVersion
                      << "\nUsage: SparkLauncher [--version] [--help] [--validate-launch-request <project> "
                         "[--binary-directory <dir>]]\n";
            LocalFree(arguments);
            return 0;
        }
    }
    if (arguments)
        LocalFree(arguments);
#else
int main(int argc, char** argv)
try
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--validate-launch-request") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--validate-launch-request requires a project path\n";
                return 2;
            }
            const auto projectFile = std::filesystem::u8path(argv[++i]);
            std::filesystem::path binaryDirectory;
            if (i + 1 < argc)
            {
                if (std::strcmp(argv[i + 1], "--binary-directory") != 0 || i + 2 >= argc || i + 3 != argc)
                {
                    std::cerr << "Unexpected argument after --validate-launch-request\n";
                    return 2;
                }
                binaryDirectory = std::filesystem::u8path(argv[i += 2]);
            }
            return ValidateLaunchRequest(projectFile, binaryDirectory);
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0)
        {
            std::cout << kVersion << '\n';
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::cout << kVersion
                      << "\nUsage: SparkLauncher [--version] [--help] [--validate-launch-request <project> "
                         "[--binary-directory <dir>]]\n";
            return 0;
        }
    }
#endif

    if (!SparkLauncher::Backend_Init(1000, 640, "Spark Launcher"))
    {
        std::cerr << "Failed to initialize launcher backend\n";
        return 1;
    }

    SparkLauncher::LauncherApp app;
    if (!app.Initialize())
    {
        SparkLauncher::Backend_Shutdown();
        return 2;
    }

    while (!app.ShouldClose() && SparkLauncher::Backend_BeginFrame())
    {
        app.DrawUI();
        SparkLauncher::Backend_EndFrame();
    }

    SparkLauncher::Backend_Shutdown();
    return 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "SparkLauncher failed: %.512s\n", error.what());
    return 1;
}
catch (...)
{
    std::fputs("SparkLauncher failed: unexpected exception\n", stderr);
    return 1;
}
