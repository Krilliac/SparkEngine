/**
 * @file CrashHandler.h
 * @brief Unhandled-exception crash handler with local process-dump generation
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a configurable crash-handling system that installs a Windows
 * Structured Exception Handling (SEH) unhandled-exception filter. When the
 * application crashes or an assertion fails, the handler can:
 *
 * - Generate a minimal Windows process dump (.dmp) for post-mortem debugging
 * - Capture a screenshot of the last rendered frame
 * - Collect system information (OS version, GPU, memory, etc.)
 * - Dump all thread call stacks for multi-threaded diagnosis
 * - Publish a local manifest and hand it to the read-only SparkCrashReporter
 *
 * The engine never uploads a report and accepts no transport credentials.
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
 * report is presented. Reports stay local; there is no transport configuration.
 */
struct CrashConfig
{
    std::wstring dumpPrefix = L"GameEngineCrash"; ///< Filename prefix for generated process-dump files
    bool captureScreenshot = true;                ///< Whether to capture a screenshot at crash time
    bool captureSystemInfo = true;                ///< Whether to collect OS/GPU/memory information
    bool captureAllThreads = true;                ///< Whether to dump call stacks for all threads
    bool captureFullMemoryDump = false;           ///< Opt-in full-memory dump; always kept local
    bool triggerCrashOnAssert = false;            ///< Whether assertion failures should generate a full crash report
    bool requireConsent = true;                   ///< Ask before a screenshot is packaged with the report
    bool headlessMode = false;                    ///< Skip all dialog boxes and the reporter (CI/testing/headless)
    bool promptUserDescription = true;            ///< Show "what were you doing" text input after crash
    bool allowScreenshotRefusal = true;           ///< Let users refuse screenshots in consent dialog

    // Populated at crash time by user input (not configured in settings)
    std::string userDescription = ""; ///< User-provided crash description (filled at crash time)
};

/**
 * @brief Install the crash handler with the given configuration
 *
 * On Windows: registers an SEH unhandled-exception filter that catches crashes
 * and generates minimal process dumps unless local-only full-memory capture was explicitly enabled. On Linux:
 * installs signal handlers for SIGSEGV, SIGFPE, SIGABRT, etc. On other platforms: a no-op stub.
 *
 * @param cfg Configuration controlling crash report behavior
 */
void InstallCrashHandler(const CrashConfig& cfg);

/**
 * @brief Called by Assert::Fail to optionally trigger a crash report
 *
 * If CrashConfig::triggerCrashOnAssert is true, this generates a full crash
 * report including a process dump and system information. Otherwise, it only logs
 * the assertion message without generating a crash dump.
 *
 * @param assertMsg The formatted assertion failure message
 */
void TriggerCrashHandler(const char* assertMsg);

/**
 * @brief Write a crash report now, regardless of CrashConfig::triggerCrashOnAssert
 *
 * For failures the process does not survive and that raise no exception the
 * unhandled-exception filter can see: a watchdog-detected freeze followed by
 * _Exit(), and a fatal assertion followed by abort(). Those paths must leave a
 * dump and manifest on disk, so they must not go through the assert toggle —
 * that toggle only exists as a developer convenience for surviving asserts.
 *
 * @param reason Human-readable description recorded in the report
 */
void TriggerCrashReport(const char* reason);

/**
 * @brief Write a crash report with no user interaction and no upload
 *
 * For failures detected while nobody can answer a dialog and the caller is about
 * to end the process anyway — the freeze watchdog is the case that matters: its
 * whole purpose is to kill a hung game, so it must not wait on a consent prompt
 * or a bounded-but-slow upload on its way to _Exit(). This writes the dump, the
 * log and the manifest and returns; transport is left to the out-of-process
 * reporter, or to the pending-manifest sweep on a later launch.
 *
 * Like TriggerCrashReport(), this is subject to the one-report-per-process rule:
 * whichever entry point runs first owns the report.
 *
 * @param reason Human-readable description recorded in the report
 */
void TriggerCrashReportUnattended(const char* reason);

/**
 * @brief Toggle whether assertion failures generate crash reports at runtime
 *
 * Allows runtime control over the CrashConfig::triggerCrashOnAssert setting
 * without reinstalling the crash handler.
 *
 * @param shouldCrash true to generate crash reports on assert, false to only log
 */
void SetAssertCrashBehavior(bool shouldCrash);
