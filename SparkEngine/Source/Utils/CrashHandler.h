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

    // GitHub Issue upload — creates an issue on the repo's Issues tab with the
    // crash log embedded as text and the zip/dump uploaded as a release asset
    // linked from the issue body. Requires NETWORKING_ENABLED.
    std::string githubRepo = "";                  ///< GitHub repo in "owner/repo" format (empty = disabled)
    std::string githubToken = "";                 ///< GitHub personal access token (PAT) with repo/issues scope
    std::string githubLabels = "crash-report";    ///< Comma-separated labels to apply to the created issue
    bool githubAttachDump = true;                 ///< Whether to upload the zip/dump as a release asset and link it
};

/**
 * @brief Install the crash handler with the given configuration
 *
 * On Windows: registers an SEH unhandled-exception filter that catches crashes
 * and generates minidumps. On Linux: installs signal handlers for SIGSEGV,
 * SIGFPE, SIGABRT, etc. On other platforms: a no-op stub.
 *
 * @param cfg Configuration controlling crash report behavior
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
