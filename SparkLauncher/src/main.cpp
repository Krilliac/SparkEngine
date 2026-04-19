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

namespace
{
    constexpr const char* kVersion = "SparkLauncher 1.0.0";
}

#ifdef _WIN32
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int)
{
    if (cmdLine && std::strstr(cmdLine, "--version"))
    {
        MessageBoxA(nullptr, kVersion, "SparkLauncher", MB_OK);
        return 0;
    }
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
            std::cout << kVersion << "\nUsage: SparkLauncher [--version]\n";
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
