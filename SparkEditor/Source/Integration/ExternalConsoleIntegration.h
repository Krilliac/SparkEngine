/**
 * @file ExternalConsoleIntegration.h
 * @brief External console integration for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>

namespace SparkEditor
{

    /**
 * @brief External console integration for connecting to Spark Engine
 */
    class ExternalConsoleIntegration
    {
      public:
        struct ConsoleMessage
        {
            std::string level;
            std::string message;
            std::string timestamp;
        };

        using MessageCallback = std::function<void(const ConsoleMessage&)>;

      public:
        ExternalConsoleIntegration();
        ~ExternalConsoleIntegration();

        /**
     * @brief Initialize the console integration
     * @return true if successful
     */
        bool Initialize();

        /**
     * @brief Shutdown the console integration
     */
        void Shutdown();

        /**
     * @brief Connect to external Spark Engine
     * @param host Host address (default: localhost)
     * @param port Port number (default: 7777)
     * @return true if connection successful
     */
        bool ConnectToEngine(const std::string& host = "localhost", int port = 7777);

        /**
     * @brief Disconnect from engine
     */
        void Disconnect();

        /**
     * @brief Check if connected to engine
     * @return true if connected
     */
        bool IsConnected() const { return m_connected; }

        /**
     * @brief Send command to engine
     * @param command Command string
     * @return true if sent successfully
     */
        bool SendCommand(const std::string& command);

        /**
     * @brief Log message to console (engine-style logging)
     * @param message Message to log
     * @param level Log level (INFO, ERROR, SUCCESS, WARNING, etc.)
     */
        void LogToConsole(const std::string& message, const std::string& level = "INFO");

        /**
     * @brief Register callback for receiving messages
     * @param callback Function to call when message received
     */
        void SetMessageCallback(MessageCallback callback);

        /**
     * @brief Get recent messages
     * @return Vector of recent console messages
     */
        std::vector<ConsoleMessage> GetRecentMessages() const;

        /**
     * @brief Clear message history
     */
        void ClearMessages();

      private:
        void NetworkThread();
        void ProcessIncomingData();
        void SimulateEngineConnection();
        std::string GetCurrentTimestamp();

        /**
     * @brief Launch external SparkConsole.exe process
     * @return true if launched successfully
     */
        bool LaunchExternalConsole();

        /**
     * @brief Send message to external console
     * @param message Message to send
     * @return true if sent successfully
     */
        bool SendMessageToConsole(const std::string& message);

        /**
     * @brief Read response from external console
     * @return true if data was read
     */
        bool ReadFromConsole();

      private:
        std::atomic<bool> m_connected{false};
        std::atomic<bool> m_running{false};
        std::thread m_networkThread;
        mutable std::mutex m_messagesMutex;

        std::vector<ConsoleMessage> m_messages;
        MessageCallback m_messageCallback;

        std::string m_host;
        int m_port;

        // External console process handles
#ifdef _WIN32
        void* m_consoleProcess = nullptr; // HANDLE
        void* m_stdinWrite = nullptr;     // HANDLE
        void* m_stdoutRead = nullptr;     // HANDLE
#endif
    };

} // namespace SparkEditor