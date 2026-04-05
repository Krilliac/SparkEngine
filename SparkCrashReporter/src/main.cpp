/**
 * @file main.cpp
 * @brief SparkCrashReporter entry point
 *
 * Usage:
 *   SparkCrashReporter --watch <manifest-dir> <engine-pid>
 *       Watchdog mode: monitor engine process, show dialog on crash.
 *
 *   SparkCrashReporter --report <manifest-file>
 *       Direct mode: show dialog and upload from a crash manifest.
 *
 *   SparkCrashReporter --help
 *       Show usage information.
 */

#include "CrashReporterApp.h"

#include <cstring>
#include <iostream>
#include <string>

static void PrintUsage(const char* argv0)
{
    std::cerr << "SparkCrashReporter — Out-of-process crash report handler\n\n";
    std::cerr << "Usage:\n";
    std::cerr << "  " << argv0 << " --watch <manifest-dir> <engine-pid>\n";
    std::cerr << "      Watchdog mode: monitor engine, report crashes.\n\n";
    std::cerr << "  " << argv0 << " --report <manifest-file>\n";
    std::cerr << "      Direct mode: show dialog and upload from manifest.\n\n";
    std::cerr << "  " << argv0 << " --help\n";
    std::cerr << "      Show this message.\n";
}

int main(int argc, char* argv[])
{
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

    if (mode == "--watch" && argc >= 4)
    {
        std::string manifestDir = argv[2];
        std::string enginePID = argv[3];
        return SparkCrashReporter::WatchAndReport(manifestDir, enginePID);
    }

    if (mode == "--report" && argc >= 3)
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
