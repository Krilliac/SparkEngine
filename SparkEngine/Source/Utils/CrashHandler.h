/**
 * @file CrashHandler.h
 * @brief Unhandled-exception crash handler with minidump generation and upload
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a configurable crash-handling system that installs a Windows
 * Structured Exception Handling (SEH) unhandled-exception filter. When the
 * application crashes or an assertion fails, the handler can:
 *
 * - Generate a minidump (.dmp) file for post-mortem debugging
 * - Capture a screenshot of the last rendered frame
 * - Collect system information (OS version, GPU, memory, etc.)
 * - Dump all thread call stacks for multi-threaded diagnosis
 * - Optionally compress and upload the crash report to a remote server
 *
 * The crash handler also integrates with the Assert system (see Assert.h):
 * when an assertion fails, TriggerCrashHandler() is called, which can either
 * generate a full crash report or simply log the failure depending on
 * CrashConfig::triggerCrashOnAssert.
 *
 * Typical usage:
 * @code
 *   CrashConfig cfg;
 *   cfg.dumpPrefix = L"SparkEngine";
 *   cfg.uploadURL  = "https://crashes.example.com/upload";
 *   InstallCrashHandler(cfg);
 * @endcode
 *
 * @note The crash handler must be installed early in application startup,
 *       ideally before any graphics or audio initialization.
 * @see Assert, Assert::Fail, DEBUG_BREAK
 */

#pragma once
#include "../Core/Platform.h"
#include <string>

/**
 * @brief Configuration options for the crash handling system
 *
 * Controls what data is captured when a crash occurs and how the crash
 * report is processed (local storage, compression, upload).
 */
struct CrashConfig
{
    std::wstring dumpPrefix = L"GameEngineCrash"; ///< Filename prefix for generated minidump files
    std::string uploadURL = "";                   ///< Remote URL to upload crash reports (empty = no upload)
    bool captureScreenshot = true;                ///< Whether to capture a screenshot at crash time
    bool captureSystemInfo = true;                ///< Whether to collect OS/GPU/memory information
    bool captureAllThreads = true;                ///< Whether to dump call stacks for all threads
    bool zipBeforeUpload = true;                  ///< Whether to compress the report before uploading
    bool triggerCrashOnAssert = false;            ///< Whether assertion failures should generate a full crash report
    int connectTimeoutSeconds = 5;                ///< HTTP connection timeout for crash report uploads
};

#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @brief Install the unhandled-exception filter with the given configuration
 *
 * Registers a Windows SEH filter that catches unhandled exceptions and generates
 * crash reports according to the provided configuration. This should be called
 * once at application startup.
 *
 * @param cfg Configuration controlling crash report behavior
 *
 * @note Calling this function multiple times will replace the previously
 *       installed handler.
 */
void InstallCrashHandler(const CrashConfig& cfg);

/**
 * @brief Called by Assert::Fail to optionally trigger a crash report
 *
 * If CrashConfig::triggerCrashOnAssert is true, this generates a full crash
 * report including minidump and system information. Otherwise, it only logs
 * the assertion message without generating a crash dump.
 *
 * @param assertMsg The formatted assertion failure message
 *
 * @see Assert::Fail, SetAssertCrashBehavior
 */
void TriggerCrashHandler(const char* assertMsg);

/**
 * @brief Toggle whether assertion failures generate crash reports at runtime
 *
 * Allows runtime control over the CrashConfig::triggerCrashOnAssert setting
 * without reinstalling the crash handler.
 *
 * @param shouldCrash true to generate crash reports on assert, false to only log
 */
void SetAssertCrashBehavior(bool shouldCrash);

#else // !SPARK_PLATFORM_WINDOWS

// Stub implementations for non-Windows platforms
inline void InstallCrashHandler(const CrashConfig&) {}
inline void TriggerCrashHandler(const char*) {}
inline void SetAssertCrashBehavior(bool) {}

#endif // SPARK_PLATFORM_WINDOWS
