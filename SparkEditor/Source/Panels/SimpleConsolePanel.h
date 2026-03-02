/**
 * @file SimpleConsolePanel_AutoConnect.h
 * @brief Enhanced simple console panel with auto-connect functionality
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../Integration/ExternalConsoleIntegration.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace SparkEditor {

class SimpleConsolePanel : public EditorPanel {
public:
    struct LogMessage {
        std::string level;
        std::string message;
        std::string timestamp;
    };

public:
    SimpleConsolePanel();
    ~SimpleConsolePanel() override;

    // EditorPanel overrides
    bool Initialize() override;
    void Update(float deltaTime) override;
    void Render() override;
    void Shutdown() override;
    bool HandleEvent(const std::string& eventType, void* eventData) override;

    /**
     * @brief Add log message to console
     * @param level Log level (INFO, WARNING, ERROR)
     * @param message Message text
     */
    void AddMessage(const std::string& level, const std::string& message);

    /**
     * @brief Execute command
     * @param command Command string
     * @return Command result
     */
    std::string ExecuteCommand(const std::string& command);

    /**
     * @brief Clear console
     */
    void Clear();

    /**
     * @brief Connect to external console
     * @return true if connection successful
     */
    bool ConnectToExternalConsole();

    /**
     * @brief Disconnect from external console
     */
    void DisconnectFromExternalConsole();

    /**
     * @brief Set auto-connect preference
     * @param autoConnect Whether to auto-connect on startup
     */
    void SetAutoConnect(bool autoConnect);

    /**
     * @brief Get auto-connect preference
     * @return Current auto-connect setting
     */
    bool GetAutoConnect() const { return m_autoConnect; }

private:
    void RenderMessages();
    void RenderCommandInput();
    void RenderExternalConsoleControls();
    void RenderAutoConnectSettings();
    std::string GetCurrentTimestamp();
    void RegisterBuiltInCommands();
    void OnExternalConsoleMessage(const ExternalConsoleIntegration::ConsoleMessage& msg);
    void SaveSettings();
    void LoadSettings();
    bool IsDebuggerSafe();

private:
    std::vector<LogMessage> m_messages;
    std::vector<std::string> m_commandHistory;
    char m_commandBuffer[512];
    bool m_scrollToBottom = true;
    bool m_showInfo = true;
    bool m_showWarning = true;
    bool m_showError = true;
    bool m_showSuccess = true;
    int m_infoCount = 0;
    int m_warningCount = 0;
    int m_errorCount = 0;
    bool m_externalConsoleConnected = false;
    bool m_autoConnect = false;  // Default to false for debugger compatibility
    bool m_autoConnectAttempted = false;
    bool m_debuggerDetected = false;
    
    std::function<std::string(const std::vector<std::string>&)> m_helpCommand;
    std::function<std::string(const std::vector<std::string>&)> m_clearCommand;
    std::function<std::string(const std::vector<std::string>&)> m_echoCommand;
    
    // External console integration
    std::unique_ptr<ExternalConsoleIntegration> m_externalConsole;
};

} // namespace SparkEditor