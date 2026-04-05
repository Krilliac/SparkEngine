/**
 * @file CrashReporterApp.h
 * @brief Out-of-process crash report handler
 *
 * Standalone application launched by SparkEngine at startup. Monitors the
 * engine process and, when a crash is detected, shows a user-facing dialog
 * with consent, description input, screenshot preview, and handles upload.
 *
 * Communication:
 *   Engine writes a crash manifest file (JSON) to a known path, then signals
 *   the reporter via a named event (Windows) or pipe (Linux). The manifest
 *   contains paths to the minidump, log, screenshot, and zip files.
 *
 * Why out-of-process:
 *   - Survives total process corruption (heap trashed, stack overflow)
 *   - Can show a proper GUI with text input (not just MessageBox buttons)
 *   - Non-blocking: engine process can terminate immediately
 *   - More reliable upload delivery
 */

#pragma once

#include <string>

namespace SparkCrashReporter
{

    /// Crash manifest written by the engine, read by the reporter
    struct CrashManifest
    {
        std::string enginePID;              ///< PID of the crashed engine process
        std::string timestamp;              ///< ISO 8601 crash timestamp
        std::string dumpFile;               ///< Path to minidump (.dmp or .core_hint)
        std::string logFile;                ///< Path to crash log (.log)
        std::string screenshotFile;         ///< Path to screenshot (.png), empty if none
        std::string zipFile;                ///< Path to compressed archive (.zip)
        std::string crashTitle;             ///< Short crash summary (e.g., "SIGSEGV")
        std::string uploadURL;              ///< Upload URL (auto-detect backend)
        std::string proxyURL;               ///< Proxy relay URL
        std::string githubRepo;             ///< GitHub repo for direct API
        std::string githubToken;            ///< GitHub PAT (dev builds)
        std::string githubLabels;           ///< Issue labels
        std::string smtpUser;               ///< SMTP username
        std::string smtpPass;               ///< SMTP password
        std::string emailTo;                ///< Email recipient
        std::string emailFrom;              ///< Email sender
        bool requireConsent = true;         ///< Show consent dialog
        bool allowScreenshotRefusal = true; ///< Let user refuse screenshot
        bool promptUserDescription = true;  ///< Show description input
        int timeoutSeconds = 5;             ///< Upload timeout
    };

    /// Parse a crash manifest from a JSON file
    bool LoadManifest(const std::string& path, CrashManifest& out);

    /// Write a crash manifest to a JSON file
    bool WriteManifest(const std::string& path, const CrashManifest& manifest);

    /// Run the crash reporter GUI and upload flow
    /// Returns 0 on success, non-zero on error
    int RunCrashReporter(const CrashManifest& manifest);

    /// Watch for the engine process to crash (used in watchdog mode)
    /// Returns when the engine exits or a crash manifest appears
    int WatchAndReport(const std::string& manifestDir, const std::string& enginePID);

} // namespace SparkCrashReporter
