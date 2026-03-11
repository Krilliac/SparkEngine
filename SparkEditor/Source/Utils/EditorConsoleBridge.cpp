/**
 * @file EditorConsoleBridge.cpp
 * @brief Implementation of EditorConsoleBridge (R7.3 — console consolidation)
 */

#include "EditorConsoleBridge.h"

// Include the editor's local SimpleConsole header (links within SparkEditor)
#include "SparkConsole.h"

#include <sstream>

namespace SparkEditor
{

    bool EditorConsoleBridge::Initialize()
    {
        if (m_initialized)
            return true;

        // Get the engine's singleton console
        m_engineConsole = &Spark::SimpleConsole::GetInstance();

        if (!m_engineConsole)
        {
            return false;
        }

        RegisterDefaultEditorCommands();

        m_initialized = true;
        LogSuccess("[R7.3] EditorConsoleBridge connected to engine console");
        return true;
    }

    void EditorConsoleBridge::Shutdown()
    {
        if (!m_initialized)
            return;

        LogInfo("EditorConsoleBridge shutting down");
        m_engineConsole = nullptr;
        m_initialized = false;
    }

    // ========================================================================
    // Logging — forwarded to engine console
    // ========================================================================

    void EditorConsoleBridge::Log(const std::string& message, const std::string& type)
    {
        if (m_engineConsole)
        {
            m_engineConsole->Log("[Editor] " + message, type);
        }
    }

    void EditorConsoleBridge::LogInfo(const std::string& message)
    {
        Log(message, "INFO");
    }

    void EditorConsoleBridge::LogWarning(const std::string& message)
    {
        Log(message, "WARNING");
    }

    void EditorConsoleBridge::LogError(const std::string& message)
    {
        Log(message, "ERROR");
    }

    void EditorConsoleBridge::LogSuccess(const std::string& message)
    {
        Log(message, "SUCCESS");
    }

    // ========================================================================
    // Commands — registered on engine console with "editor." prefix
    // ========================================================================

    void EditorConsoleBridge::RegisterEditorCommand(const std::string& name, CommandHandler handler,
                                                    const std::string& description)
    {
        if (!m_engineConsole)
            return;

        std::string fullName = "editor." + name;
        m_engineConsole->RegisterCommand(fullName, handler, "[Editor] " + description);
    }

    bool EditorConsoleBridge::ExecuteCommand(const std::string& commandLine)
    {
        if (!m_engineConsole)
            return false;

        return m_engineConsole->ExecuteCommand(commandLine);
    }

    // ========================================================================
    // Default editor commands
    // ========================================================================

    void EditorConsoleBridge::RegisterDefaultEditorCommands()
    {
        RegisterEditorCommand(
            "status",
            [this](const std::vector<std::string>& /*args*/) -> std::string
            {
                std::stringstream ss;
                ss << "EditorConsoleBridge Status:\n";
                ss << "  Connected to engine console: " << (m_engineConsole ? "YES" : "NO") << "\n";
                ss << "  Initialized: " << (m_initialized ? "YES" : "NO") << "\n";
                ss << "  Console consolidation (R7.3): ACTIVE\n";
                return ss.str();
            },
            "Display editor console bridge status");

        RegisterEditorCommand(
            "version", [](const std::vector<std::string>& /*args*/) -> std::string
            { return "SparkEditor v1.0.0 - Console Bridge (R7.3)\nAll logging consolidated through engine console"; },
            "Display editor version");

        RegisterEditorCommand(
            "test_logging",
            [this](const std::vector<std::string>& /*args*/) -> std::string
            {
                LogInfo("Test INFO message from editor");
                LogWarning("Test WARNING message from editor");
                LogError("Test ERROR message from editor");
                LogSuccess("Test SUCCESS message from editor");
                return "Editor test messages sent through engine console";
            },
            "Send test messages through the consolidated console");
    }

} // namespace SparkEditor
