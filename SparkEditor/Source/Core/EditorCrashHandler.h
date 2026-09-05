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
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace SparkEditor
{

    class EditorLogger;

    /**
 * @brief Crash information structure
 */
    struct CrashInfo
    {
#ifdef _WIN32
        EXCEPTION_POINTERS* exceptionPointers = nullptr;
#else
        int signalNumber = 0;
#endif
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
        static EditorCrashHandler& GetInstance();

        bool Initialize(const std::string& crashDirectory = "Crashes", EditorLogger* logger = nullptr);
        void Shutdown();

        void SetCrashCallback(CrashCallback callback);
        void SetRecoveryCallback(RecoveryCallback callback);
        void SetAssertCallback(AssertCallback callback);

        void HandleAssertion(const std::string& expression, const char* file, int line,
                             const std::string& message = "");

        void RecordOperation(const std::string& operation);
        void SetEditorState(const std::string& state);

        bool SaveRecoveryData();
        std::optional<RecoveryData> LoadRecoveryData();
        bool HasRecoveryData();
        void ClearRecoveryData();

        void SetAutoSaveRecovery(bool enabled, float interval = 30.0f);

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

        std::string GenerateCrashReport(const CrashInfo& crashInfo);

        void TestCrashHandler();
        void TestAssertionHandler();

      private:
        EditorCrashHandler() = default;
        ~EditorCrashHandler();

        // Non-copyable
        EditorCrashHandler(const EditorCrashHandler&) = delete;
        EditorCrashHandler& operator=(const EditorCrashHandler&) = delete;

#ifdef _WIN32
        static LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS* exceptionPointers);
        void HandleCrashInternal(EXCEPTION_POINTERS* exceptionPointers);
        std::string GenerateStackTrace(EXCEPTION_POINTERS* exceptionPointers);
        bool SaveCrashDump(EXCEPTION_POINTERS* exceptionPointers, const std::string& filePath);
#else
        static void SignalHandler(int signal);
#endif

        std::string GetSystemInfo();
        std::string GetThreadInfo();
        bool SaveCrashLog(const CrashInfo& crashInfo, const std::string& filePath);
        void UpdateStats(const CrashInfo& crashInfo);
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
#ifdef _WIN32
        LPTOP_LEVEL_EXCEPTION_FILTER m_previousFilter = nullptr;
        bool m_filterInstalled = false; ///< True while our unhandled-exception filter is installed.
#endif
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
