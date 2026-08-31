/**
 * @file main.cpp
 * @brief SparkLauncher entry point — Unity-Hub-style project picker that spawns SparkEditor.
 * @author Spark Engine Team
 * @date 2025
 */

#include "LauncherApp.h"
#include "LauncherBackend.h"

#include <cstring>
#include <iostream>
#include <string_view>

namespace
{
#ifndef SPARK_LAUNCHER_VERSION
#error "SPARK_LAUNCHER_VERSION must be supplied by the build system"
#endif
    constexpr const char* kVersion = "SparkLauncher " SPARK_LAUNCHER_VERSION;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int)
{
    (void)cmdLine;
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    for (int index = 1; arguments && index < argumentCount; ++index)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--version" || argument == L"-v")
        {
            std::cout << kVersion << '\n';
            LocalFree(arguments);
            return 0;
        }
        if (argument == L"--help" || argument == L"-h")
        {
            std::cout << kVersion << "\nUsage: SparkLauncher [--version] [--help]\n";
            LocalFree(arguments);
            return 0;
        }
    }
    if (arguments)
        LocalFree(arguments);
#else
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0)
        {
            std::cout << kVersion << '\n';
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::cout << kVersion << "\nUsage: SparkLauncher [--version] [--help]\n";
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
