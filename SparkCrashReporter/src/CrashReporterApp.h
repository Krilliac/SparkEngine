/**
 * @file CrashReporterApp.h
 * @brief Out-of-process crash report handler
 *
 * Standalone application launched by SparkEngine at startup. Monitors the
 * engine process and, when a crash is detected, shows a user-facing dialog
 * with consent, description input, screenshot selection, and local report
 * preparation. Network delivery is not implemented by this executable.
 *
 * Communication:
 *   Engine writes a crash manifest file (JSON) to a known path, then signals
 *   the reporter via a named event (Windows) or pipe (Linux). The manifest
 *   contains paths to the process dump, log, screenshot, and zip files.
 *
 * Why out-of-process:
 *   - Survives total process corruption (heap trashed, stack overflow)
 *   - Can show a proper GUI with text input (not just MessageBox buttons)
 *   - Non-blocking: engine process can terminate immediately
 *   - Reliable crash-artifact collection after the engine exits
 */

#pragma once

#include <cstdint>
#include <string>

namespace SparkCrashReporter
{
    struct ArtifactIdentity
    {
        std::uint64_t device = 0;
        std::uint64_t file = 0;
        bool valid = false;
    };

    /// Crash manifest written by the engine, read by the reporter
    struct CrashManifest
    {
        std::string enginePID;              ///< PID of the crashed engine process
        std::string timestamp;              ///< ISO 8601 crash timestamp
        std::string dumpFile;               ///< Path to process dump (.dmp or .core_hint)
        std::string logFile;                ///< Path to crash log (.log)
        std::string screenshotFile;         ///< Path to screenshot (.png), empty if none
        std::string zipFile;                ///< Path to compressed archive (.zip)
        std::string crashTitle;             ///< Short crash summary (e.g., "SIGSEGV")
        std::string uploadURL;              ///< Legacy input accepted then securely discarded by the reader
        std::string proxyURL;               ///< Legacy input accepted then securely discarded by the reader
        std::string githubRepo;             ///< Legacy input accepted then securely discarded by the reader
        std::string githubToken;            ///< Legacy input accepted then securely discarded by the reader
        std::string githubLabels;           ///< Legacy input accepted then securely discarded by the reader
        std::string smtpUser;               ///< Legacy input accepted then securely discarded by the reader
        std::string smtpPass;               ///< Legacy input accepted then securely discarded by the reader
        std::string emailTo;                ///< Legacy input accepted then securely discarded by the reader
        std::string emailFrom;              ///< Legacy input accepted then securely discarded by the reader
        bool requireConsent = true;         ///< Show consent dialog
        bool allowScreenshotRefusal = true; ///< Let user refuse screenshot
        bool promptUserDescription = true;  ///< Show description input
        bool fullMemoryDump = false;        ///< Dump contains full process memory (explicit engine opt-in)
        int timeoutSeconds = 5;             ///< Legacy input is validated then reset to this inert default

        // Trusted local state populated by LoadManifest. It is deliberately
        // never read from or written to manifest JSON.
        std::string artifactRoot; ///< Pinned directory that confines every artifact path
        ArtifactIdentity artifactRootIdentity;
        ArtifactIdentity logIdentity;
        ArtifactIdentity dumpIdentity;
        ArtifactIdentity screenshotIdentity;
        ArtifactIdentity zipIdentity;
    };

    /// Parse a crash manifest from a JSON file
    bool LoadManifest(const std::string& path, CrashManifest& out);

    /// Write a crash manifest to a JSON file
    bool WriteManifest(const std::string& path, const CrashManifest& manifest);

    /// Build the privacy disclosure shown before crash-report consent.
    std::string BuildConsentMessage(const CrashManifest& manifest);

    /// Run the crash reporter UI and prepare the local report
    /// Returns 0 on success, non-zero on error
    int RunCrashReporter(const CrashManifest& manifest);

    /// Watch for the engine process to crash (used in watchdog mode)
    /// Consumes sequential manifests and returns after the engine exits
    int WatchAndReport(const std::string& manifestDir, const std::string& enginePID);

} // namespace SparkCrashReporter
