/**
 * @file EditorConsoleBridge.h
 * @brief Bridge that connects the editor's SimpleConsole to the engine's authoritative console (R7.3)
 *
 * ## Problem (R7.3 — Architecture Analysis)
 * SparkEditor/Source/Utils/SparkConsole.h contains a duplicate SimpleConsole class that
 * reimplements logging, command registration, and history management already present in
 * the engine's SparkEngine/Source/Utils/SparkConsole.h. This leads to:
 * - Divergent behavior between engine and editor console
 * - Duplicated bug fixes
 * - Two separate command registries
 *
 * ## Solution
 * EditorConsoleBridge acts as a thin adapter that forwards editor console operations
 * to the engine's authoritative SimpleConsole instance. The editor's duplicate class
 * remains for backward compatibility but new code should use the bridge.
 *
 * ## Usage
 * @code
 *   // During editor initialization:
 *   auto& bridge = EditorConsoleBridge::GetInstance();
 *   bridge.Initialize();
 *
 *   // Logging goes through the engine's console
 *   bridge.LogInfo("Editor panel loaded");
 *
 *   // Commands are registered on the engine's console
 *   bridge.RegisterEditorCommand("editor.reload", handler, "Reload editor panels");
 * @endcode
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

// Forward declare the engine console to avoid header coupling
namespace Spark
{
    class SimpleConsole;
}

namespace SparkEditor
{

    /**
     * @brief Singleton bridge connecting editor logging/commands to the engine console.
     *
     * All editor subsystems should use this bridge instead of directly instantiating
     * the editor's local SimpleConsole. This ensures a single source of truth for
     * command registration, log history, and CVar management.
     */
    class EditorConsoleBridge
    {
      public:
        using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

        static EditorConsoleBridge& GetInstance()
        {
            static EditorConsoleBridge instance;
            return instance;
        }

        /**
         * @brief Connect to the engine's SimpleConsole.
         *
         * Must be called after the engine console is initialized.
         * Registers editor-specific default commands on the engine console.
         *
         * @return true on success.
         */
        bool Initialize();

        /**
         * @brief Disconnect from the engine console.
         */
        void Shutdown();

        // ====================================================================
        // Logging — delegates to engine's SimpleConsole
        // ====================================================================

        void Log(const std::string& message, const std::string& type = "INFO");
        void LogInfo(const std::string& message);
        void LogWarning(const std::string& message);
        void LogError(const std::string& message);
        void LogSuccess(const std::string& message);

        // ====================================================================
        // Command registration — registers on engine's console with "editor." prefix
        // ====================================================================

        /**
         * @brief Register an editor command on the engine console.
         *
         * Commands are automatically prefixed with "editor." to namespace them.
         * For example, registering "reload" creates the command "editor.reload".
         */
        void RegisterEditorCommand(const std::string& name, CommandHandler handler,
                                   const std::string& description = "");

        /**
         * @brief Execute a command through the engine console.
         */
        bool ExecuteCommand(const std::string& commandLine);

        /**
         * @brief Check if the bridge is connected to the engine console.
         */
        bool IsConnected() const { return m_engineConsole != nullptr; }

      private:
        EditorConsoleBridge() = default;
        ~EditorConsoleBridge() = default;
        EditorConsoleBridge(const EditorConsoleBridge&) = delete;
        EditorConsoleBridge& operator=(const EditorConsoleBridge&) = delete;

        Spark::SimpleConsole* m_engineConsole = nullptr;
        bool m_initialized = false;

        void RegisterDefaultEditorCommands();
    };

} // namespace SparkEditor
