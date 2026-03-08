/**
 * @file EditorCrashHandler.h
 * @brief Enhanced crash handling system for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <mutex>
#include <thread>
#include <Windows.h>

namespace SparkEditor
{

    class EditorLogger;

    /**
 * @brief Crash information structure
 */
    struct CrashInfo
    {
        EXCEPTION_POINTERS* exceptionPointers = nullptr;
        std::string exceptionType;
        std::string exceptionMessage;
        std::string stackTrace;
        std::string threadInfo;
        std::string systemInfo;
        std::string editorState;
        std::string lastOperations;
        std::chrono::system_clock::time_point timestamp;
        uint32_t processId = 0;
        uint32_t threadId = 0;
    };

    /**
 * @brief Crash recovery data
 */
    struct RecoveryData
    {
        std::string currentLayout;
        std::vector<std::string> openFiles;
        std::string currentProject;
        std::string lastSavedScene;
        std::unordered_map<std::string, std::string> editorSettings;
        std::vector<std::string> recentOperations;
    };

    /**
 * @brief Crash handler callback types
 */
    using CrashCallback = std::function<void(const CrashInfo&)>;
    using RecoveryCallback = std::function<RecoveryData()>;
    using AssertCallback = std::function<void(const std::string&, const std::string&, int, const std::string&)>;

    /**
 * @brief Enhanced crash handling system for Spark Engine Editor
 * 
 * Provides comprehensive crash handling with:
 * - Integration with engine crash handler
 * - Editor state preservation
 * - Automatic recovery data saving
 * - Detailed crash reporting
 * - Assert handling integration
 */
    class EditorCrashHandler
    {
      public:
        /**
     * @brief Get singleton instance
     * @return Reference to crash handler instance
     */
        static EditorCrashHandler& GetInstance();

        /**
     * @brief Initialize crash handler
     * @param crashDirectory Directory for crash dumps and logs
     * @param logger Editor logger instance
     * @return true if initialization succeeded
     */
        bool Initialize(const std::string& crashDirectory = "Crashes", EditorLogger* logger = nullptr);

        /**
     * @brief Shutdown crash handler
     */
        void Shutdown();

        /**
     * @brief Set crash callback
     * @param callback Function to call when crash occurs
     */
        void SetCrashCallback(CrashCallback callback);

        /**
     * @brief Set recovery data callback
     * @param callback Function to call to gather recovery data
     */
        void SetRecoveryCallback(RecoveryCallback callback);

        /**
     * @brief Set assert callback
     * @param callback Function to call when assertion fails
     */
        void SetAssertCallback(AssertCallback callback);

        /**
     * @brief Handle assertion failure
     * @param expression Failed expression
     * @param file Source file
     * @param line Line number
     * @param message Optional message
     */
        void HandleAssertion(const std::string& expression, const char* file, int line,
                             const std::string& message = "");

        /**
     * @brief Add operation to recent operations list
     * @param operation Description of operation
     */
        void RecordOperation(const std::string& operation);

        /**
     * @brief Set current editor state
     * @param state Current state description
     */
        void SetEditorState(const std::string& state);

        /**
     * @brief Save recovery data immediately
     * @return true if save succeeded
     */
        bool SaveRecoveryData();

        /**
     * @brief Load recovery data from last session
     * @return Recovery data if available
     */
        std::optional<RecoveryData> LoadRecoveryData();

        /**
     * @brief Check if recovery data is available
     * @return true if recovery data exists
     */
        bool HasRecoveryData();

        /**
     * @brief Clear recovery data
     */
        void ClearRecoveryData();

        /**
     * @brief Enable/disable automatic recovery data saving
     * @param enabled Enable auto-save
     * @param interval Save interval in seconds
     */
        void SetAutoSaveRecovery(bool enabled, float interval = 30.0f);

        /**
     * @brief Get crash statistics
     */
        struct CrashStats
        {
            int totalCrashes = 0;
            int assertionFailures = 0;
            int accessViolations = 0;
            int stackOverflows = 0;
            int otherExceptions = 0;
            std::chrono::system_clock::time_point lastCrash;
            std::string lastCrashType;
            float averageSessionTime = 0.0f;
            int recoveryDataSaves = 0;
            int successfulRecoveries = 0;
        };
        CrashStats GetStats() const;

        /**
     * @brief Generate crash report
     * @param crashInfo Crash information
     * @return Formatted crash report
     */
        std::string GenerateCrashReport(const CrashInfo& crashInfo);

        /**
     * @brief Test crash handler (debug only)
     */
        void TestCrashHandler();

        /**
     * @brief Test assertion handler (debug only)
     */
        void TestAssertionHandler();

      private:
        EditorCrashHandler() = default;
        ~EditorCrashHandler();

        // Non-copyable
        EditorCrashHandler(const EditorCrashHandler&) = delete;
        EditorCrashHandler& operator=(const EditorCrashHandler&) = delete;

        /**
     * @brief Windows exception filter
     */
        static LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS* exceptionPointers);

        /**
     * @brief Handle crash internally
     */
        void HandleCrashInternal(EXCEPTION_POINTERS* exceptionPointers);

        /**
     * @brief Generate stack trace
     */
        std::string GenerateStackTrace(EXCEPTION_POINTERS* exceptionPointers);

        /**
     * @brief Get system information
     */
        std::string GetSystemInfo();

        /**
     * @brief Get thread information
     */
        std::string GetThreadInfo();

        /**
     * @brief Save crash dump
     */
        bool SaveCrashDump(EXCEPTION_POINTERS* exceptionPointers, const std::string& filePath);

        /**
     * @brief Save crash log
     */
        bool SaveCrashLog(const CrashInfo& crashInfo, const std::string& filePath);

        /**
     * @brief Update statistics
     */
        void UpdateStats(const CrashInfo& crashInfo);

        /**
     * @brief Auto-save recovery data thread function
     */
        void AutoSaveRecoveryThread();

      private:
        // State
        bool m_initialized = false;
        std::string m_crashDirectory;
        EditorLogger* m_logger = nullptr;

        // Callbacks
        CrashCallback m_crashCallback;
        RecoveryCallback m_recoveryCallback;
        AssertCallback m_assertCallback;

        // Recent operations tracking
        std::vector<std::string> m_recentOperations;
        std::string m_currentEditorState;
        size_t m_maxOperations = 50;
        mutable std::mutex m_operationsMutex;

        // Auto-save recovery
        bool m_autoSaveEnabled = true;
        float m_autoSaveInterval = 30.0f;
        std::atomic<bool> m_shouldStopAutoSave{false};
        std::thread m_autoSaveThread;

        // Statistics
        mutable CrashStats m_stats;
        std::chrono::steady_clock::time_point m_sessionStartTime;
        mutable std::mutex m_statsMutex;

        // Exception handling
        LPTOP_LEVEL_EXCEPTION_FILTER m_previousFilter = nullptr;
        static EditorCrashHandler* s_instance;
    };

} // namespace SparkEditor

// Integration macros for easier use
#define EDITOR_RECORD_OPERATION(op) SparkEditor::EditorCrashHandler::GetInstance().RecordOperation(op)

#define EDITOR_SET_STATE(state) SparkEditor::EditorCrashHandler::GetInstance().SetEditorState(state)

#define EDITOR_ASSERT(expr)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            SparkEditor::EditorCrashHandler::GetInstance().HandleAssertion(#expr, __FILE__, __LINE__);                 \
        }                                                                                                              \
    } while (0)

#define EDITOR_ASSERT_MSG(expr, msg)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            SparkEditor::EditorCrashHandler::GetInstance().HandleAssertion(#expr, __FILE__, __LINE__, msg);            \
        }                                                                                                              \
    } while (0)