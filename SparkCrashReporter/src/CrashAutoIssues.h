#pragma once

#include "CrashReporterApp.h"

#include <filesystem>
#include <string>

namespace SparkCrashReporter
{
    // User-local opt-in. Crash manifests cannot enable or configure delivery.
    bool AutoIssuesEnabled();
    bool SetAutoIssuesEnabled(bool enabled);

    // Local-only stable key for one pinned crash log. Never transmitted.
    std::string CrashReceiptKey(const CrashManifest& manifest);
    // Cryptographically random public correlation code; empty on entropy failure.
    std::string GeneratePublicIncidentId();

    struct AutoIssueResult
    {
        bool delivered = false;
        std::string issueUrl;
        std::string reason;
    };

    struct PreparedAutoIssue
    {
        bool ready = false;
        std::filesystem::path ghExecutable;
        std::filesystem::path workingDirectory;
        std::string body;
        std::string reason;
    };

    // All certain local failures are resolved before the one-shot receipt is claimed.
    PreparedAutoIssue PrepareAutoIssue(const CrashManifest& manifest, const std::string& incidentId);
    // Network-facing step; only call after atomically claiming the receipt.
    AutoIssueResult SubmitPreparedAutoIssue(const PreparedAutoIssue& prepared);

    // Read-only subprocess diagnostic for the test executable. Does not contact Issues.
    std::string ProbeGhVersion(const std::filesystem::path& ghExecutable);
} // namespace SparkCrashReporter
