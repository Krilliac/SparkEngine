/**
 * @file main.cpp
 * @brief SparkEditor entry point with integrated debug and collab server support
 * @author Spark Engine Team
 * @date 2025
 */

#include "Core/EditorApplication.h"
#include "Communication/CollaborativeEditSession.h"
#include "Core/FaultIsolation.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include <iostream>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

static std::atomic<bool> g_collabServerRunning{true};

static void CollabServerSignalHandler(int /*sig*/)
{
    g_collabServerRunning.store(false, std::memory_order_release);
}

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
                try
                {
                    int pid = std::stoi(line.substr(10));
                    return pid != 0;
                }
                catch (const std::exception&)
                {
                    return false;
                }
            }
        }
    }
    return false;
#endif
}

#ifndef _WIN32
#include <fstream>
#endif

/// @brief Run as a headless collaborative editing server (no GUI).
/// Usage: SparkEditor --collab-server [--port PORT] [--name NAME]
static int RunCollabServer(uint16_t port, const std::string& serverName)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.Initialize();

    std::signal(SIGINT, CollabServerSignalHandler);
    std::signal(SIGTERM, CollabServerSignalHandler);

    console.LogInfo("Starting headless collab server on port " + std::to_string(port) + " as '" + serverName + "'...");
    std::cout << "=== SPARKEDITOR COLLAB SERVER ===" << std::endl;
    std::cout << "Port: " << port << "  Name: " << serverName << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;

    SparkEditor::CollaborativeEditSession session;

    session.SetPeerConnectedCallback(
        [&console](const SparkEditor::EditorPeer& peer)
        { console.LogInfo("Peer connected: " + peer.userName + " (ID=" + std::to_string(peer.id) + ")"); });

    session.SetPeerDisconnectedCallback([&console](SparkEditor::PeerID id)
                                        { console.LogInfo("Peer disconnected: ID=" + std::to_string(id)); });

    session.SetEditReceivedCallback([&console](const SparkEditor::EditMessage& edit)
                                    { console.LogInfo("Edit: " + edit.nodeId + "." + edit.propertyName); });

    if (!session.Host(port, serverName))
    {
        console.LogError("Failed to start collab server on port " + std::to_string(port));
        std::cerr << "ERROR: Failed to bind on port " << port << std::endl;
        console.Shutdown();
        return 1;
    }

    console.LogSuccess("Collab server running. Waiting for editors to connect...");
    std::cout << "Server started successfully." << std::endl;

    // Headless main loop — just tick the session at 10 Hz
    while (g_collabServerRunning.load(std::memory_order_acquire))
    {
        SPARK_GUARDED_UPDATE("Editor:CollabSession", "Editor", { session.Update(0.1f); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Periodic status output
        auto stats = session.GetStats();
        if (static_cast<int>(stats.sessionDuration) % 30 == 0 && stats.sessionDuration > 0.0f)
        {
            std::cout << "[" << static_cast<int>(stats.sessionDuration) << "s] Peers: " << stats.peerCount
                      << " Locks: " << stats.activeLocks << " Edits: " << stats.editsBroadcast << "/"
                      << stats.editsReceived << std::endl;
        }
    }

    std::cout << std::endl << "Shutting down collab server..." << std::endl;
    session.Disconnect();
    console.LogInfo("Collab server stopped.");
    console.Shutdown();
    return 0;
}

// Unified main entry point with debug support when needed
int main(int argc, char* argv[])
{
    // Initialize the Spark console system first (like the engine does)
    auto& console = Spark::SimpleConsole::GetInstance();

    // Check for debug console request or if debugger is present
    bool showDebugConsole = false;
    bool collabServerMode = false;
    uint16_t collabPort = 27030;
    std::string collabServerName = "CollabServer";

    bool testMode = false;
    int testFrameLimit = 0; // 0 = run indefinitely

    std::string projectPathArg;
    std::string startupTheme;

    // Check command line arguments
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--debug-console") == 0 || strcmp(argv[i], "-d") == 0)
        {
            showDebugConsole = true;
        }
        else if (strcmp(argv[i], "--collab-server") == 0)
        {
            collabServerMode = true;
        }
        else if (strcmp(argv[i], "--collab-port") == 0 && i + 1 < argc)
        {
            collabPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "--collab-name") == 0 && i + 1 < argc)
        {
            collabServerName = argv[++i];
        }
        else if (strcmp(argv[i], "--test-mode") == 0)
        {
            testMode = true;
            showDebugConsole = true;
        }
        else if (strcmp(argv[i], "--test-frames") == 0 && i + 1 < argc)
        {
            testFrameLimit = std::atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc)
        {
            projectPathArg = argv[++i];
        }
        else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc)
        {
            startupTheme = argv[++i];
        }
    }

    // Headless collab server mode — no GUI, just networking
    if (collabServerMode)
    {
        return RunCollabServer(collabPort, collabServerName);
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "SparkEditor console system initialized");
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
        config.projectPath = projectPathArg.empty() ? "." : projectPathArg;
        config.enableLogging = true;
        config.startMaximized = false; // Don't start maximized in debug mode
        config.windowWidth = 1600;
        config.windowHeight = 900;
        config.testMode = testMode;
        config.testFrameLimit = testFrameLimit;
        config.startupTheme = startupTheme;

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Editor config: %dx%d testMode=%s testFrames=%d", config.windowWidth,
                       config.windowHeight, testMode ? "true" : "false", testFrameLimit);
        console.LogInfo("Editor configuration prepared");

        if (showDebugConsole)
        {
            std::cout << "Calling app->Initialize()..." << std::endl;
        }

        console.LogInfo("Initializing SparkEditor application...");
        if (!app->Initialize(config))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to initialize SparkEditor application");
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

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down SparkEditor application");
        console.LogInfo("Shutting down SparkEditor application...");
        // Cleanup
        app->Shutdown();
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "SparkEditor shutdown complete");
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
        SPARK_LOG_FATAL(Spark::LogCategory::Editor, "SparkEditor exception: %s", e.what());
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
        SPARK_LOG_FATAL(Spark::LogCategory::Editor, "SparkEditor: Unknown critical exception");
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
