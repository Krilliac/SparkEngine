/**
 * @file SimpleConsolePanel_EngineIntegration.cpp
 * @brief Enhanced implementation with engine-style logging integration
 * @author Spark Engine Team
 * @date 2025
 */

#include "SimpleConsolePanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../Utils/SparkConsole.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <cstdio>
#include <fstream>
#include <filesystem>

// External function to connect SparkConsole to external console
extern "C" void SetSparkConsoleExternalConsole(SparkEditor::ExternalConsoleIntegration* console);

#ifdef _WIN32
#include <windows.h>
#include <ShlObj.h>
#endif

namespace SparkEditor
{

    SimpleConsolePanel::SimpleConsolePanel() : EditorPanel("Console", "simple_console_panel")
    {
        m_commandBuffer[0] = '\0';

        // Detect if running under debugger
#ifdef _WIN32
        m_debuggerDetected = IsDebuggerPresent();
#endif
    }

    SimpleConsolePanel::~SimpleConsolePanel() {}

    bool SimpleConsolePanel::Initialize()
    {
        SPARK_TRACE_ENTER(LogCategory::Editor);
        std::cout << "Initializing Enhanced Console panel with SparkConsole integration\n";

        // Initialize SparkConsole system
        auto& sparkConsole = Spark::SimpleConsole::GetInstance();
        sparkConsole.LogInfo("SimpleConsolePanel initializing...");

        // Load settings first
        LoadSettings();

        // Initialize external console integration with enhanced safety
        try
        {
            m_externalConsole = std::unique_ptr<ExternalConsoleIntegration>(new ExternalConsoleIntegration());
            m_externalConsole->Initialize();

            // Set up message callback
            m_externalConsole->SetMessageCallback([this](const ExternalConsoleIntegration::ConsoleMessage& msg)
                                                  { OnExternalConsoleMessage(msg); });

            sparkConsole.LogSuccess("External console integration initialized");
        }
        catch (const std::exception& e)
        {
            sparkConsole.LogError("Failed to initialize external console integration: " + std::string(e.what()));
            m_externalConsole.reset();
        }

        RegisterBuiltInCommands();
        AddMessage("INFO", "Spark Engine Console initialized with engine-style logging");

        if (m_debuggerDetected)
        {
            AddMessage("WARNING", "Visual Studio debugger detected - external console may be unstable");
            AddMessage("INFO", "Auto-connect is disabled by default when debugger is present");
        }

        // Check if auto-connect is enabled and safe
        if (m_autoConnect && IsDebuggerSafe())
        {
            AddMessage("INFO", "Auto-connect enabled - attempting to connect to external console...");
            // Delay auto-connect slightly to let the UI settle
            m_autoConnectAttempted = false; // Will be attempted in Update()
        }
        else
        {
            AddMessage("INFO", "Auto-connect disabled - use 'Connect' button or toggle auto-connect in settings");
        }

        return true;
    }

    void SimpleConsolePanel::Update(float deltaTime)
    {
        // Handle delayed auto-connect
        if (m_autoConnect && !m_autoConnectAttempted && !m_externalConsoleConnected && IsDebuggerSafe())
        {
            static float autoConnectDelay = 0.0f;
            autoConnectDelay += deltaTime;

            // Wait 2 seconds after initialization before auto-connecting
            if (autoConnectDelay >= 2.0f)
            {
                m_autoConnectAttempted = true;
                if (ConnectToExternalConsole())
                {
                    AddMessage("SUCCESS", "Auto-connected to external console");
                }
                else
                {
                    AddMessage("WARNING", "Auto-connect failed - external console not available");
                }
            }
        }
    }

    void SimpleConsolePanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Console toolbar
            if (ImGui::Button(ICON_FA_TRASH " Clear"))
            {
                Clear();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &m_scrollToBottom);

            ImGui::SameLine();
            ImGui::Text("|");
            ImGui::SameLine();

            // Log level filter toggle buttons with counts
            m_infoCount = m_warningCount = m_errorCount = 0;
            for (const auto& msg : m_messages)
            {
                if (msg.level == "INFO" || msg.level == "RESULT" || msg.level == "COMMAND" || msg.level == "TRACE")
                    m_infoCount++;
                else if (msg.level == "WARNING")
                    m_warningCount++;
                else if (msg.level == "ERROR" || msg.level == "CRITICAL")
                    m_errorCount++;
            }

            // Info filter
            ImVec4 infoBtnColor = m_showInfo ? ImVec4(0.176f, 0.549f, 0.941f, 0.8f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, infoBtnColor);
            char infoLabel[64];
            snprintf(infoLabel, sizeof(infoLabel), ICON_FA_INFO_CIRCLE " %d", m_infoCount);
            if (ImGui::Button(infoLabel))
                m_showInfo = !m_showInfo;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toggle Info messages");

            ImGui::SameLine();

            // Warning filter
            ImVec4 warnBtnColor = m_showWarning ? ImVec4(0.961f, 0.651f, 0.137f, 0.8f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, warnBtnColor);
            char warnLabel[64];
            snprintf(warnLabel, sizeof(warnLabel), ICON_FA_EXCLAMATION " %d", m_warningCount);
            if (ImGui::Button(warnLabel))
                m_showWarning = !m_showWarning;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toggle Warning messages");

            ImGui::SameLine();

            // Error filter
            ImVec4 errBtnColor = m_showError ? ImVec4(0.898f, 0.224f, 0.208f, 0.8f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, errBtnColor);
            char errLabel[64];
            snprintf(errLabel, sizeof(errLabel), ICON_FA_TIMES " %d", m_errorCount);
            if (ImGui::Button(errLabel))
                m_showError = !m_showError;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toggle Error messages");

            ImGui::SameLine();
            ImGui::Text("|");
            ImGui::SameLine();

            // External console controls
            RenderExternalConsoleControls();

            ImGui::Separator();

            // Auto-connect settings
            RenderAutoConnectSettings();

            ImGui::Separator();

            // Messages area
            RenderMessages();

            ImGui::Separator();

            // Command input
            RenderCommandInput();
        }
        EndPanel();
    }

    void SimpleConsolePanel::Shutdown()
    {
        auto& sparkConsole = Spark::SimpleConsole::GetInstance();
        sparkConsole.LogInfo("Shutting down Enhanced Console panel");

        // Save settings
        SaveSettings();

        // Disconnect SparkConsole from external console
        SetSparkConsoleExternalConsole(nullptr);

        if (m_externalConsole)
        {
            try
            {
                m_externalConsole->Shutdown();
                m_externalConsole.reset();
            }
            catch (const std::exception& e)
            {
                sparkConsole.LogError("Exception during external console shutdown: " + std::string(e.what()));
            }
            catch (...)
            {
                sparkConsole.LogError("Unknown exception during external console shutdown");
            }
        }

        sparkConsole.LogSuccess("Enhanced Console panel shutdown complete");
    }

    bool SimpleConsolePanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    void SimpleConsolePanel::AddMessage(const std::string& level, const std::string& message)
    {
        LogMessage msg;
        msg.level = level;
        msg.message = message;
        msg.timestamp = GetCurrentTimestamp();

        m_messages.push_back(msg);

        // Limit messages
        if (m_messages.size() > 1000)
        {
            m_messages.erase(m_messages.begin());
        }

        m_scrollToBottom = true;

        // CRITICAL INTEGRATION: Also send to SparkConsole system for engine-style logging
        // This ensures all console panel messages appear in external console
        auto& sparkConsole = Spark::SimpleConsole::GetInstance();
        sparkConsole.Log(message, level);
    }

    std::string SimpleConsolePanel::ExecuteCommand(const std::string& command)
    {
        if (command.empty())
        {
            return "";
        }

        // Add to history
        m_commandHistory.push_back(command);

        // Parse command
        std::istringstream iss(command);
        std::vector<std::string> tokens;
        std::string token;

        while (iss >> token)
        {
            tokens.push_back(token);
        }

        if (tokens.empty())
        {
            return "Empty command";
        }

        std::string cmd = tokens[0];
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());

        // Handle SparkConsole integration commands
        if (cmd == "test_logging")
        {
            auto& sparkConsole = Spark::SimpleConsole::GetInstance();
            sparkConsole.LogInfo("This is a test INFO message from SimpleConsolePanel");
            sparkConsole.LogSuccess("This is a test SUCCESS message from SimpleConsolePanel");
            sparkConsole.LogWarning("This is a test WARNING message from SimpleConsolePanel");
            sparkConsole.LogError("This is a test ERROR message from SimpleConsolePanel");
            return "Test logging messages sent through SparkConsole system";
        }

        // Handle auto-connect commands
        if (cmd == "auto_connect")
        {
            if (args.empty())
            {
                return "Auto-connect is " + std::string(m_autoConnect ? "enabled" : "disabled");
            }
            else if (args[0] == "on" || args[0] == "true" || args[0] == "1")
            {
                SetAutoConnect(true);
                return "Auto-connect enabled (will take effect on next startup)";
            }
            else if (args[0] == "off" || args[0] == "false" || args[0] == "0")
            {
                SetAutoConnect(false);
                return "Auto-connect disabled";
            }
            else
            {
                return "Usage: auto_connect [on|off|true|false|1|0]";
            }
        }

        // Handle external console commands
        if (cmd == "connect_console")
        {
            if (ConnectToExternalConsole())
            {
                return "Connected to external SparkConsole.exe with engine-style logging";
            }
            else
            {
                return "Failed to connect to external console";
            }
        }
        else if (cmd == "disconnect_console")
        {
            DisconnectFromExternalConsole();
            return "Disconnected from external console";
        }
        else if (cmd == "send_to_console" && !args.empty())
        {
            if (m_externalConsoleConnected && m_externalConsole)
            {
                std::string message;
                for (size_t i = 0; i < args.size(); ++i)
                {
                    if (i > 0)
                        message += " ";
                    message += args[i];
                }
                if (m_externalConsole->SendCommand(message))
                {
                    return "Sent to external console: " + message;
                }
                else
                {
                    return "Failed to send to external console";
                }
            }
            else
            {
                return "Not connected to external console";
            }
        }

        // Try SparkConsole system commands first
        auto& sparkConsole = Spark::SimpleConsole::GetInstance();
        if (sparkConsole.ExecuteCommand(command))
        {
            return ""; // SparkConsole already logged the result
        }

        // Execute built-in commands
        if (cmd == "help")
        {
            return m_helpCommand(args);
        }
        else if (cmd == "clear")
        {
            return m_clearCommand(args);
        }
        else if (cmd == "echo")
        {
            return m_echoCommand(args);
        }
        else if (cmd == "status")
        {
            std::string status = "Console Status: Active | Messages: " + std::to_string(m_messages.size());
            if (m_externalConsoleConnected)
            {
                status += " | External Console: Connected";
            }
            else
            {
                status += " | External Console: Not Connected";
            }
            status += " | Auto-connect: " + std::string(m_autoConnect ? "Enabled" : "Disabled");
            if (m_debuggerDetected)
            {
                status += " | Debugger: Detected";
            }
            return status;
        }

        // If connected to external console, forward unknown commands
        if (m_externalConsoleConnected && m_externalConsole)
        {
            try
            {
                if (m_externalConsole->SendCommand(command))
                {
                    return "Command forwarded to external console";
                }
            }
            catch (const std::exception& e)
            {
                return "Error forwarding command: " + std::string(e.what());
            }
        }

        return "Unknown command: " + cmd + ". Type 'help' for available commands.";
    }

    bool SimpleConsolePanel::ConnectToExternalConsole()
    {
        if (!m_externalConsole)
        {
            AddMessage("ERROR", "External console integration not available");
            return false;
        }

        auto& sparkConsole = Spark::SimpleConsole::GetInstance();

        // Extra safety check for debugger
        if (m_debuggerDetected)
        {
            AddMessage("WARNING", "Connecting to external console while debugger is attached");
            AddMessage("INFO", "This may cause instability - consider running without debugger");
        }

        try
        {
            if (m_externalConsole->ConnectToEngine())
            {
                m_externalConsoleConnected = true;
                AddMessage("SUCCESS", "Connected to external SparkConsole.exe");

                // CRITICAL FIX: Connect the SparkConsole system to the external console
                // This is the missing piece that makes engine-style logging work!
                SetSparkConsoleExternalConsole(m_externalConsole.get());

                AddMessage("SUCCESS", "SparkConsole system connected to external console");
                AddMessage("INFO", "All editor operations will now appear in external console");

                sparkConsole.LogSuccess("External console connection established");
                sparkConsole.LogInfo("Engine-style logging is now active in external console");

                return true;
            }
            else
            {
                AddMessage("ERROR", "Failed to connect to external console");
                return false;
            }
        }
        catch (const std::exception& e)
        {
            AddMessage("ERROR", "Exception during external console connection: " + std::string(e.what()));
            return false;
        }
    }

    void SimpleConsolePanel::DisconnectFromExternalConsole()
    {
        auto& sparkConsole = Spark::SimpleConsole::GetInstance();

        if (m_externalConsole)
        {
            try
            {
                // Disconnect SparkConsole from external console
                SetSparkConsoleExternalConsole(nullptr);

                m_externalConsole->Disconnect();
                m_externalConsoleConnected = false;
                AddMessage("INFO", "Disconnected from external console");

                sparkConsole.LogInfo("External console disconnected - engine-style logging disabled");
            }
            catch (const std::exception& e)
            {
                AddMessage("ERROR", "Exception during disconnection: " + std::string(e.what()));
            }
        }
    }

    void SimpleConsolePanel::SetAutoConnect(bool autoConnect)
    {
        m_autoConnect = autoConnect;
        SaveSettings();
    }

    void SimpleConsolePanel::RenderExternalConsoleControls()
    {
        if (m_externalConsoleConnected)
        {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "External Console: Connected");
            ImGui::SameLine();
            if (ImGui::Button("Disconnect"))
            {
                DisconnectFromExternalConsole();
            }
            ImGui::SameLine();
            if (ImGui::Button("Test Logging"))
            {
                auto& sparkConsole = Spark::SimpleConsole::GetInstance();
                sparkConsole.LogInfo("Test message from SparkEditor - INFO level");
                sparkConsole.LogSuccess("Test message from SparkEditor - SUCCESS level");
                sparkConsole.LogWarning("Test message from SparkEditor - WARNING level");
                sparkConsole.LogError("Test message from SparkEditor - ERROR level");
                AddMessage("INFO", "Test messages sent to external console via SparkConsole system");
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "External Console: Not Connected");
            ImGui::SameLine();
            if (ImGui::Button("Connect"))
            {
                ConnectToExternalConsole();
            }
        }

        if (!m_externalConsole)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "(Integration Not Available)");
        }

        if (m_debuggerDetected)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "[DEBUGGER DETECTED]");
        }
    }

    void SimpleConsolePanel::RenderAutoConnectSettings()
    {
        ImGui::Text("Settings:");
        ImGui::SameLine();

        bool autoConnectChanged = ImGui::Checkbox("Auto-connect on startup", &m_autoConnect);
        if (autoConnectChanged)
        {
            SaveSettings();
            if (m_autoConnect)
            {
                AddMessage("INFO", "Auto-connect enabled (will take effect on next startup)");
            }
            else
            {
                AddMessage("INFO", "Auto-connect disabled");
            }
        }

        if (m_debuggerDetected && m_autoConnect)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "(Disabled due to debugger)");
        }

        ImGui::SameLine();
        if (ImGui::Button("Save Settings"))
        {
            SaveSettings();
            AddMessage("SUCCESS", "Settings saved");
        }
    }

    void SimpleConsolePanel::OnExternalConsoleMessage(const ExternalConsoleIntegration::ConsoleMessage& msg)
    {
        try
        {
            // Don't call AddMessage here to avoid infinite recursion through SparkConsole
            LogMessage localMsg;
            localMsg.level = msg.level;
            localMsg.message = msg.message;
            localMsg.timestamp = GetCurrentTimestamp();

            m_messages.push_back(localMsg);

            // Limit messages
            if (m_messages.size() > 1000)
            {
                m_messages.erase(m_messages.begin());
            }

            m_scrollToBottom = true;
        }
        catch (const std::exception& e)
        {
            std::cout << "Exception in OnExternalConsoleMessage: " << e.what() << "\n";
        }
    }

    void SimpleConsolePanel::Clear()
    {
        m_messages.clear();
        AddMessage("INFO", "Console cleared.");
    }

    void SimpleConsolePanel::RenderMessages()
    {
        const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        // Use monospace font for log messages
        ImFont* monoFont = EditorFonts::GetMonospace();
        if (monoFont)
            ImGui::PushFont(monoFont);

        for (const auto& msg : m_messages)
        {
            // Apply filter
            bool isInfo = (msg.level == "INFO" || msg.level == "RESULT" || msg.level == "COMMAND" ||
                           msg.level == "TRACE" || msg.level == "SUCCESS");
            bool isWarning = (msg.level == "WARNING");
            bool isError = (msg.level == "ERROR" || msg.level == "CRITICAL");

            if (isInfo && !m_showInfo)
                continue;
            if (isWarning && !m_showWarning)
                continue;
            if (isError && !m_showError)
                continue;

            // Color by level
            ImVec4 color(0.88f, 0.89f, 0.92f, 1.0f); // Default: cool white
            const char* icon = ICON_FA_INFO_CIRCLE;
            if (msg.level == "WARNING")
            {
                color = ImVec4(0.96f, 0.80f, 0.28f, 1.0f);
                icon = ICON_FA_EXCLAMATION;
            }
            else if (msg.level == "ERROR")
            {
                color = ImVec4(0.90f, 0.30f, 0.28f, 1.0f);
                icon = ICON_FA_TIMES;
            }
            else if (msg.level == "CRITICAL")
            {
                color = ImVec4(1.0f, 0.45f, 0.0f, 1.0f);
                icon = ICON_FA_BOMB;
            }
            else if (msg.level == "SUCCESS")
            {
                color = ImVec4(0.30f, 0.80f, 0.32f, 1.0f);
                icon = ICON_FA_CHECK;
            }
            else if (msg.level == "COMMAND")
            {
                color = ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
                icon = ICON_FA_CHEVRON_RIGHT;
            }
            else if (msg.level == "TRACE")
            {
                color = ImVec4(0.60f, 0.60f, 0.85f, 1.0f);
                icon = ICON_FA_BUG;
            }

            ImGui::TextColored(ImVec4(0.45f, 0.48f, 0.52f, 1.0f), "[%s]", msg.timestamp.c_str());
            ImGui::SameLine();
            ImGui::TextColored(color, "%s", icon);
            ImGui::SameLine();
            ImGui::TextColored(color, "%s", msg.message.c_str());
        }

        if (monoFont)
            ImGui::PopFont();

        if (m_scrollToBottom && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
            m_scrollToBottom = false;
        }

        ImGui::EndChild();
    }

    void SimpleConsolePanel::RenderCommandInput()
    {
        bool reclaimFocus = false;

        if (ImGui::InputText("Command", m_commandBuffer, sizeof(m_commandBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            std::string command = m_commandBuffer;
            if (!command.empty())
            {
                // Add command to log
                AddMessage("COMMAND", "> " + command);

                // Execute command
                try
                {
                    std::string result = ExecuteCommand(command);
                    if (!result.empty())
                    {
                        AddMessage("RESULT", result);
                    }
                }
                catch (const std::exception& e)
                {
                    AddMessage("ERROR", "Command execution error: " + std::string(e.what()));
                }
            }

            m_commandBuffer[0] = '\0';
            reclaimFocus = true;
        }

        // Auto-focus on window apparition
        ImGui::SetItemDefaultFocus();
        if (reclaimFocus)
        {
            ImGui::SetKeyboardFocusHere(-1);
        }
    }

    std::string SimpleConsolePanel::GetCurrentTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
        ss << "." << std::setfill('0') << std::setw(3) << ms.count();

        return ss.str();
    }

    void SimpleConsolePanel::RegisterBuiltInCommands()
    {
        m_helpCommand = [](const std::vector<std::string>& args) -> std::string
        {
            return "Available commands:\n"
                   "  help - Show this help message\n"
                   "  clear - Clear console\n"
                   "  echo <text> - Echo text to console\n"
                   "  status - Show console status\n"
                   "  test_logging - Send test messages through SparkConsole system\n"
                   "  connect_console - Connect to external SparkConsole.exe\n"
                   "  disconnect_console - Disconnect from external console\n"
                   "  send_to_console <text> - Send text to external console\n"
                   "  auto_connect [on|off] - Enable/disable auto-connect on startup\n"
                   "\nSparkConsole Commands (when connected):\n"
                   "  version - Show SparkEditor version\n"
                   "  external_status - Check external console status\n"
                   "\nWhen connected to external console, unknown commands are forwarded automatically.\n"
                   "Auto-connect is disabled when Visual Studio debugger is detected.";
        };

        m_clearCommand = [this](const std::vector<std::string>& args) -> std::string
        {
            m_messages.clear();
            return "";
        };

        m_echoCommand = [](const std::vector<std::string>& args) -> std::string
        {
            std::stringstream ss;
            for (size_t i = 0; i < args.size(); ++i)
            {
                if (i > 0)
                    ss << " ";
                ss << args[i];
            }
            return ss.str();
        };
    }

    static std::string GetConsoleSettingsPath()
    {
        std::string dir;
#ifdef _WIN32
        char appDataPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath)))
        {
            dir = std::string(appDataPath) + "\\SparkEngine\\Editor";
        }
        else
        {
            dir = ".";
        }
#else
        const char* home = getenv("HOME");
        if (home)
        {
            dir = std::string(home) + "/.sparkengine/editor";
        }
        else
        {
            dir = ".";
        }
#endif
        try
        {
            std::filesystem::create_directories(dir);
        }
        catch (...)
        {
        }
        return dir + "/console_settings.txt";
    }

    void SimpleConsolePanel::SaveSettings()
    {
        try
        {
            std::string settingsPath = GetConsoleSettingsPath();
            std::ofstream file(settingsPath);
            if (file.is_open())
            {
                file << "auto_connect=" << (m_autoConnect ? "1" : "0") << std::endl;
                file.close();
            }
        }
        catch (const std::exception& e)
        {
            std::cout << "Failed to save console settings: " << e.what() << "\n";
        }
    }

    void SimpleConsolePanel::LoadSettings()
    {
        try
        {
            std::string settingsPath = GetConsoleSettingsPath();
            std::ifstream file(settingsPath);
            if (file.is_open())
            {
                std::string line;
                while (std::getline(file, line))
                {
                    if (line.find("auto_connect=") == 0)
                    {
                        std::string value = line.substr(13);
                        m_autoConnect = (value == "1" || value == "true");
                    }
                }
                file.close();
            }
        }
        catch (const std::exception& e)
        {
            std::cout << "Failed to load console settings: " << e.what() << "\n";
        }
    }

    bool SimpleConsolePanel::IsDebuggerSafe()
    {
        // Disable auto-connect when debugger is present for safety
        return !m_debuggerDetected;
    }

} // namespace SparkEditor