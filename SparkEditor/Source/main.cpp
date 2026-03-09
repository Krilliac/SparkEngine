/**
 * @file main.cpp
 * @brief SparkEditor entry point with integrated debug support
 * @author Spark Engine Team
 * @date 2025
 */

#include "Core/EditorApplication.h"
#include "Utils/SparkConsole.h" // Use local SparkConsole instead of engine version
#include <iostream>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
#include <cstring>

#ifdef _WIN32
#include <Windows.h>
#endif

static bool IsDebuggerAttached()
{
#ifdef _WIN32
    return IsDebuggerPresent() != 0;
#else
    // On Linux, check /proc/self/status for TracerPid
    std::ifstream status("/proc/self/status");
    if (status.is_open())
    {
        std::string line;
        while (std::getline(status, line))
        {
            if (line.find("TracerPid:") == 0)
            {
                int pid = std::stoi(line.substr(10));
                return pid != 0;
            }
        }
    }
    return false;
#endif
}

#ifndef _WIN32
#include <fstream>
#endif

// Unified main entry point with debug support when needed
int main(int argc, char* argv[])
{
    // Initialize the Spark console system first (like the engine does)
    auto& console = Spark::SimpleConsole::GetInstance();

    // Check for debug console request or if debugger is present
    bool showDebugConsole = false;

    // Check command line arguments for debug console
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--debug-console") == 0 || strcmp(argv[i], "-d") == 0)
        {
            showDebugConsole = true;
            break;
        }
    }

    // Also show debug console if debugger is attached
    if (IsDebuggerAttached())
    {
        showDebugConsole = true;
    }

    // Create console window for debugging if needed
    if (showDebugConsole)
    {
#ifdef _WIN32
        AllocConsole();
        FILE* pCout;
        FILE* pCerr;
        FILE* pCin;
        freopen_s(&pCout, "CONOUT$", "w", stdout);
        freopen_s(&pCerr, "CONOUT$", "w", stderr);
        freopen_s(&pCin, "CONIN$", "r", stdin);
#endif

        // Make cout, wcout, cin, wcin, wcerr, cerr, wclog and clog point to console
        std::ios::sync_with_stdio(true);

        std::cout << "=== SPARKEDITOR DEBUG MODE ===" << std::endl;
        std::cout << "Debug console enabled (debugger detected or --debug-console flag)" << std::endl;
    }

    // Initialize Spark Console system (this will connect to external console)
    if (console.Initialize())
    {
        console.LogSuccess("SparkEditor console system initialized successfully");
        console.LogInfo("External console integration active - all editor operations will be logged");
    }
    else
    {
        if (showDebugConsole)
        {
            std::cout << "Warning: Failed to initialize Spark console system" << std::endl;
        }
    }

    try
    {
        console.LogInfo("Starting SparkEditor application...");
        if (showDebugConsole)
        {
            std::cout << "Creating EditorApplication..." << std::endl;
        }

        // Create editor application
        auto app = std::make_unique<SparkEditor::EditorApplication>();
        console.LogInfo("EditorApplication instance created");

        if (showDebugConsole)
        {
            std::cout << "Creating EditorConfig..." << std::endl;
        }

        // Initialize with basic configuration
        SparkEditor::EditorConfig config;
        config.projectPath = ".";
        config.enableLogging = true;
        config.startMaximized = false; // Don't start maximized in debug mode
        config.windowWidth = 1600;
        config.windowHeight = 900;

        console.LogInfo("Editor configuration prepared");

        if (showDebugConsole)
        {
            std::cout << "Calling app->Initialize()..." << std::endl;
        }

        console.LogInfo("Initializing SparkEditor application...");
        if (!app->Initialize(config))
        {
            console.LogError("Failed to initialize SparkEditor application");
            if (showDebugConsole)
            {
                std::cerr << "Failed to initialize SparkEditor" << std::endl;
                std::cout << "Press Enter to exit..." << std::endl;
                std::cin.get();
            }
            return -1;
        }

        console.LogSuccess("SparkEditor application initialized successfully");

        if (showDebugConsole)
        {
            std::cout << "Application initialized successfully!" << std::endl;
            std::cout << "Starting main loop..." << std::endl;
        }

        console.LogInfo("Starting SparkEditor main loop...");
        // Run the editor
        int result = app->Run();

        console.LogInfo("SparkEditor main loop ended with result: " + std::to_string(result));

        if (showDebugConsole)
        {
            std::cout << "Main loop ended with result: " << result << std::endl;
            std::cout << "Shutting down..." << std::endl;
        }

        console.LogInfo("Shutting down SparkEditor application...");
        // Cleanup
        app->Shutdown();
        console.LogSuccess("SparkEditor application shutdown complete");

        if (showDebugConsole)
        {
            std::cout << "Shutdown complete." << std::endl;
            // Only wait for input if we're in debug mode and not launched from debugger
            if (!IsDebuggerAttached())
            {
                std::cout << "Press Enter to exit..." << std::endl;
                std::cin.get();
            }
        }

        // Shutdown console system
        console.LogInfo("Shutting down SparkEditor console system...");
        console.Shutdown();

        return result;
    }
    catch (const std::exception& e)
    {
        console.LogCritical("SparkEditor exception: " + std::string(e.what()));
        if (showDebugConsole)
        {
            std::cerr << "SparkEditor error: " << e.what() << std::endl;
            std::cout << "Press Enter to exit..." << std::endl;
            std::cin.get();
        }
        console.Shutdown();
        return -1;
    }
    catch (...)
    {
        console.LogCritical("SparkEditor: Unknown critical exception occurred");
        if (showDebugConsole)
        {
            std::cerr << "SparkEditor: Unknown error occurred" << std::endl;
            std::cout << "Press Enter to exit..." << std::endl;
            std::cin.get();
        }
        console.Shutdown();
        return -1;
    }
}

#ifdef _WIN32
// WinMain entry point for Windows applications
// This is required by the linker when building as a Windows application
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
    // Parse command line arguments from lpCmdLine
    int argc = 0;
    char** argv = nullptr;

    // Simple command line parsing - convert lpCmdLine to argc/argv format
    std::string cmdLine(lpCmdLine);
    std::vector<std::string> args;
    std::vector<char*> argPtrs;

    // Add program name as first argument
    args.push_back("SparkEditor.exe");

    // Parse command line
    if (!cmdLine.empty())
    {
        std::istringstream iss(cmdLine);
        std::string arg;
        while (iss >> arg)
        {
            args.push_back(arg);
        }
    }

    // Create argv array
    for (auto& arg : args)
    {
        argPtrs.push_back(const_cast<char*>(arg.c_str()));
    }
    argPtrs.push_back(nullptr);

    argc = static_cast<int>(args.size());
    argv = argPtrs.data();

    // Call main function
    return main(argc, argv);
}
#endif // _WIN32
