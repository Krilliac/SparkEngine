/**
 * @file main.cpp
 * @brief SparkCrashReporter entry point
 *
 * Usage:
 *   SparkCrashReporter --watch <manifest-dir> <engine-pid>
 *       Watchdog mode: monitor engine process, show dialog on crash.
 *
 *   SparkCrashReporter --report <manifest-file>
 *       Direct mode: review a crash manifest, then optionally post metadata.
 *
 *   SparkCrashReporter --enable-auto-issues | --disable-auto-issues | --auto-issues-status
 *       Manage the user's persistent opt-in for public metadata-only Issues.
 *
 *   SparkCrashReporter --help
 *       Show usage information.
 */

#include "CrashReporterApp.h"
#include "CrashAutoIssues.h"

#include <cstring>
#include <iostream>
#include <string>

#ifndef SPARK_CRASH_REPORTER_VERSION
#error "SPARK_CRASH_REPORTER_VERSION must be supplied by the build system"
#endif

static void PrintUsage(const char* argv0)
{
    std::cerr << "SparkCrashReporter — Out-of-process crash report handler\n\n";
    std::cerr << "Usage:\n";
    std::cerr << "  " << argv0 << " --watch <manifest-dir> <engine-pid>\n";
    std::cerr << "      Watchdog mode: monitor engine, report crashes.\n\n";
    std::cerr << "  " << argv0 << " --report <manifest-file>\n";
    std::cerr << "      Direct mode: review local artifacts, optionally post safe metadata.\n\n";
    std::cerr << "  " << argv0 << " --enable-auto-issues\n";
    std::cerr << "      Opt in to public GitHub Issues after crash review; requires authenticated gh.\n";
    std::cerr << "  " << argv0 << " --disable-auto-issues\n";
    std::cerr << "      Revoke future automatic issue attempts.\n";
    std::cerr << "  " << argv0 << " --auto-issues-status\n";
    std::cerr << "      Show whether automatic issue submission is enabled.\n";
    std::cerr << "  " << argv0 << " --issue-status <crash-dir>\n";
    std::cerr << "      Show saved GitHub Issue outcomes for a private crash directory.\n\n";
    std::cerr << "  " << argv0 << " --help\n";
    std::cerr << "      Show this message.\n";
}

int main(int argc, char* argv[])
{
    if (argc == 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-version"))
    {
        std::cout << "SparkCrashReporter " SPARK_CRASH_REPORTER_VERSION "\n";
        return 0;
    }

    if (argc < 2)
    {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "--help" || mode == "-h")
    {
        PrintUsage(argv[0]);
        return 0;
    }

    if (argc == 2 && mode == "--enable-auto-issues")
    {
        if (!SparkCrashReporter::SetAutoIssuesEnabled(true))
        {
            std::cerr << "Could not enable automatic GitHub Issues in user-local settings.\n";
            return 2;
        }
        std::cout << "Automatic GitHub Issues enabled. Issues are public. Only reporter version, platform, "
                     "a known crash class (or Unknown), and an opaque incident ID are sent; no logs, dumps, "
                     "screenshots, paths, or descriptions. "
                     "Run --disable-auto-issues to revoke this opt-in. Existing authenticated gh is required.\n";
        return 0;
    }
    if (argc == 2 && mode == "--disable-auto-issues")
    {
        if (!SparkCrashReporter::SetAutoIssuesEnabled(false))
        {
            std::cerr << "Could not disable automatic GitHub Issues in user-local settings.\n";
            return 2;
        }
        std::cout << "Automatic GitHub Issues disabled for future crash reports.\n";
        return 0;
    }
    if (argc == 2 && mode == "--auto-issues-status")
    {
        std::cout << "Automatic GitHub Issues: " << (SparkCrashReporter::AutoIssuesEnabled() ? "enabled" : "disabled")
                  << "\n";
        return 0;
    }
    if (argc == 3 && mode == "--issue-status")
        return SparkCrashReporter::ShowAutoIssueStatus(argv[2]);

    if (mode == "--watch" && argc == 4)
    {
        std::string manifestDir = argv[2];
        std::string enginePID = argv[3];
        return SparkCrashReporter::WatchAndReport(manifestDir, enginePID);
    }

    if (mode == "--report" && argc == 3)
    {
        std::string manifestPath = argv[2];
        SparkCrashReporter::CrashManifest manifest;
        if (!SparkCrashReporter::LoadManifest(manifestPath, manifest))
        {
            std::cerr << "Error: Failed to load manifest from " << manifestPath << "\n";
            return 1;
        }
        return SparkCrashReporter::RunCrashReporter(manifest);
    }

    std::cerr << "Error: Unknown mode '" << mode << "'\n\n";
    PrintUsage(argv[0]);
    return 1;
}
