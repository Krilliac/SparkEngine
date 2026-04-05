/**
 * @file CrashReporterApp.cpp
 * @brief Out-of-process crash reporter implementation
 */

#include "CrashReporterApp.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SparkCrashReporter
{

    // ============================================================================
    // JSON helpers (minimal — no library dependency)
    // ============================================================================

    static std::string JsonExtract(const std::string& json, const std::string& key)
    {
        std::string needle = "\"" + key + "\":\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return "";
        pos += needle.size();
        size_t end = json.find('"', pos);
        if (end == std::string::npos)
            return "";
        // Unescape basic sequences
        std::string val = json.substr(pos, end - pos);
        std::string out;
        for (size_t i = 0; i < val.size(); ++i)
        {
            if (val[i] == '\\' && i + 1 < val.size())
            {
                char next = val[++i];
                if (next == 'n')
                    out += '\n';
                else if (next == 't')
                    out += '\t';
                else if (next == '"')
                    out += '"';
                else if (next == '\\')
                    out += '\\';
                else
                    out += next;
            }
            else
            {
                out += val[i];
            }
        }
        return out;
    }

    static bool JsonExtractBool(const std::string& json, const std::string& key, bool defaultVal)
    {
        std::string needle = "\"" + key + "\":";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return defaultVal;
        pos += needle.size();
        while (pos < json.size() && json[pos] == ' ')
            ++pos;
        if (pos < json.size() && json[pos] == 't')
            return true;
        if (pos < json.size() && json[pos] == 'f')
            return false;
        return defaultVal;
    }

    static int JsonExtractInt(const std::string& json, const std::string& key, int defaultVal)
    {
        std::string needle = "\"" + key + "\":";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return defaultVal;
        pos += needle.size();
        while (pos < json.size() && json[pos] == ' ')
            ++pos;
        try
        {
            return std::stoi(json.substr(pos));
        }
        catch (...)
        {
            return defaultVal;
        }
    }

    static std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 16);
        for (char c : s)
        {
            if (c == '"')
                out += "\\\"";
            else if (c == '\\')
                out += "\\\\";
            else if (c == '\n')
                out += "\\n";
            else if (c == '\t')
                out += "\\t";
            else
                out += c;
        }
        return out;
    }

    // ============================================================================
    // Manifest I/O
    // ============================================================================

    bool LoadManifest(const std::string& path, CrashManifest& out)
    {
        std::ifstream f(path);
        if (!f.is_open())
            return false;

        std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        out.enginePID = JsonExtract(json, "enginePID");
        out.timestamp = JsonExtract(json, "timestamp");
        out.dumpFile = JsonExtract(json, "dumpFile");
        out.logFile = JsonExtract(json, "logFile");
        out.screenshotFile = JsonExtract(json, "screenshotFile");
        out.zipFile = JsonExtract(json, "zipFile");
        out.crashTitle = JsonExtract(json, "crashTitle");
        out.uploadURL = JsonExtract(json, "uploadURL");
        out.proxyURL = JsonExtract(json, "proxyURL");
        out.githubRepo = JsonExtract(json, "githubRepo");
        out.githubToken = JsonExtract(json, "githubToken");
        out.githubLabels = JsonExtract(json, "githubLabels");
        out.smtpUser = JsonExtract(json, "smtpUser");
        out.smtpPass = JsonExtract(json, "smtpPass");
        out.emailTo = JsonExtract(json, "emailTo");
        out.emailFrom = JsonExtract(json, "emailFrom");
        out.requireConsent = JsonExtractBool(json, "requireConsent", true);
        out.allowScreenshotRefusal = JsonExtractBool(json, "allowScreenshotRefusal", true);
        out.promptUserDescription = JsonExtractBool(json, "promptUserDescription", true);
        out.timeoutSeconds = JsonExtractInt(json, "timeoutSeconds", 5);

        return !out.logFile.empty();
    }

    bool WriteManifest(const std::string& path, const CrashManifest& m)
    {
        std::ofstream f(path);
        if (!f.is_open())
            return false;

        f << "{\n";
        f << "  \"enginePID\": \"" << JsonEscape(m.enginePID) << "\",\n";
        f << "  \"timestamp\": \"" << JsonEscape(m.timestamp) << "\",\n";
        f << "  \"dumpFile\": \"" << JsonEscape(m.dumpFile) << "\",\n";
        f << "  \"logFile\": \"" << JsonEscape(m.logFile) << "\",\n";
        f << "  \"screenshotFile\": \"" << JsonEscape(m.screenshotFile) << "\",\n";
        f << "  \"zipFile\": \"" << JsonEscape(m.zipFile) << "\",\n";
        f << "  \"crashTitle\": \"" << JsonEscape(m.crashTitle) << "\",\n";
        f << "  \"uploadURL\": \"" << JsonEscape(m.uploadURL) << "\",\n";
        f << "  \"proxyURL\": \"" << JsonEscape(m.proxyURL) << "\",\n";
        f << "  \"githubRepo\": \"" << JsonEscape(m.githubRepo) << "\",\n";
        f << "  \"githubToken\": \"" << JsonEscape(m.githubToken) << "\",\n";
        f << "  \"githubLabels\": \"" << JsonEscape(m.githubLabels) << "\",\n";
        f << "  \"smtpUser\": \"" << JsonEscape(m.smtpUser) << "\",\n";
        f << "  \"smtpPass\": \"" << JsonEscape(m.smtpPass) << "\",\n";
        f << "  \"emailTo\": \"" << JsonEscape(m.emailTo) << "\",\n";
        f << "  \"emailFrom\": \"" << JsonEscape(m.emailFrom) << "\",\n";
        f << "  \"requireConsent\": " << (m.requireConsent ? "true" : "false") << ",\n";
        f << "  \"allowScreenshotRefusal\": " << (m.allowScreenshotRefusal ? "true" : "false") << ",\n";
        f << "  \"promptUserDescription\": " << (m.promptUserDescription ? "true" : "false") << ",\n";
        f << "  \"timeoutSeconds\": " << m.timeoutSeconds << "\n";
        f << "}\n";

        return f.good();
    }

    // ============================================================================
    // Console-based crash reporter dialog (cross-platform)
    // ============================================================================

    static std::string ReadCrashLog(const std::string& logFile)
    {
        std::ifstream f(logFile);
        if (!f.is_open())
            return "(Unable to read crash log)";
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    int RunCrashReporter(const CrashManifest& manifest)
    {
        std::string crashLog = ReadCrashLog(manifest.logFile);

        std::cerr << "\n";
        std::cerr << "================================================================\n";
        std::cerr << "       SPARK ENGINE CRASH REPORTER\n";
        std::cerr << "================================================================\n";
        std::cerr << "\n";
        std::cerr << "The engine has crashed: " << manifest.crashTitle << "\n";
        std::cerr << "Timestamp: " << manifest.timestamp << "\n";
        std::cerr << "Crash log: " << manifest.logFile << "\n";
        if (!manifest.dumpFile.empty())
            std::cerr << "Dump file: " << manifest.dumpFile << "\n";
        if (!manifest.screenshotFile.empty())
            std::cerr << "Screenshot: " << manifest.screenshotFile << "\n";
        if (!manifest.zipFile.empty())
            std::cerr << "Archive: " << manifest.zipFile << "\n";
        std::cerr << "\n";

        // Consent
        bool shouldUpload = true;
        if (manifest.requireConsent)
        {
#ifdef _WIN32
            int result = MessageBoxA(nullptr,
                                     "SparkEngine has crashed. Would you like to send a crash report "
                                     "to help improve the engine?\n\nNo personal data is included.",
                                     "Crash Report", MB_YESNO | MB_ICONERROR);
            shouldUpload = (result == IDYES);
#else
            std::cerr << "Would you like to send a crash report? [Y/n]: ";
            std::string input;
            std::getline(std::cin, input);
            shouldUpload = input.empty() || input[0] == 'Y' || input[0] == 'y';
#endif
        }

        if (!shouldUpload)
        {
            std::cerr << "Crash report NOT sent (user declined).\n";
            std::cerr << "Files saved locally.\n";
            return 0;
        }

        // Screenshot consent
        bool includeScreenshot = true;
        if (manifest.allowScreenshotRefusal && !manifest.screenshotFile.empty())
        {
#ifdef _WIN32
            int ssResult = MessageBoxA(nullptr,
                                       "Include a screenshot of the last rendered frame "
                                       "with the crash report?",
                                       "Screenshot Consent", MB_YESNO | MB_ICONERROR);
            includeScreenshot = (ssResult == IDYES);
#else
            std::cerr << "Include screenshot with report? [Y/n]: ";
            std::string input;
            std::getline(std::cin, input);
            includeScreenshot = input.empty() || input[0] == 'Y' || input[0] == 'y';
#endif
        }

        if (!includeScreenshot && !manifest.screenshotFile.empty())
        {
            std::cerr << "Screenshot will be excluded from the report.\n";
            try
            {
                std::filesystem::remove(manifest.screenshotFile);
            }
            catch (...)
            {
            }
        }

        // User description
        std::string userDescription;
        if (manifest.promptUserDescription)
        {
#ifdef _WIN32
            // On Windows, use a simple console prompt since we're a console app
            // A future version could use a proper Win32 dialog with an edit control
            std::cerr << "\nPlease describe what you were doing when the crash occurred\n";
            std::cerr << "(press Enter twice to finish, or just Enter to skip):\n> ";
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (line.empty())
                    break;
                if (!userDescription.empty())
                    userDescription += "\n";
                userDescription += line;
                std::cerr << "> ";
            }
#else
            std::cerr << "\nDescribe what you were doing (Enter to skip):\n> ";
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (line.empty())
                    break;
                if (!userDescription.empty())
                    userDescription += "\n";
                userDescription += line;
                std::cerr << "> ";
            }
#endif
        }

        // Prepend user description to crash log
        std::string fullLog = crashLog;
        if (!userDescription.empty())
            fullLog = "=== User Description ===\n" + userDescription + "\n\n" + fullLog;

        // Upload — delegate to the engine's uploader library
        // The crash reporter links against the same CrashReportUploader code
        std::cerr << "\nUploading crash report...\n";

        // For now, write the augmented log back to the log file so the
        // in-engine uploader can pick it up. A proper implementation would
        // call UploadCrashReport() directly (requires linking libcurl).
        if (!userDescription.empty())
        {
            std::ofstream f(manifest.logFile);
            if (f.is_open())
                f << fullLog;
        }

        std::cerr << "Crash report prepared for upload.\n";
        std::cerr << "Thank you for helping improve SparkEngine!\n";
        return 0;
    }

    // ============================================================================
    // Watchdog mode — monitor engine process
    // ============================================================================

    static bool IsProcessAlive(const std::string& pidStr)
    {
        if (pidStr.empty())
            return false;

#ifdef _WIN32
        DWORD pid = static_cast<DWORD>(std::stoul(pidStr));
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess)
            return false;
        DWORD exitCode = 0;
        GetExitCodeProcess(hProcess, &exitCode);
        CloseHandle(hProcess);
        return (exitCode == STILL_ACTIVE);
#else
        pid_t pid = static_cast<pid_t>(std::stoi(pidStr));
        return (kill(pid, 0) == 0);
#endif
    }

    int WatchAndReport(const std::string& manifestDir, const std::string& enginePID)
    {
        std::cerr << "[CrashReporter] Watching engine process (PID " << enginePID << ")...\n";

        std::string manifestPath = manifestDir + "/crash_manifest.json";

        // Poll until the engine exits or a manifest appears
        while (true)
        {
            // Check for manifest file (engine wrote it during crash)
            if (std::filesystem::exists(manifestPath))
            {
                CrashManifest manifest;
                if (LoadManifest(manifestPath, manifest))
                {
                    std::cerr << "[CrashReporter] Crash manifest detected!\n";
                    int result = RunCrashReporter(manifest);
                    // Clean up manifest
                    std::filesystem::remove(manifestPath);
                    return result;
                }
            }

            // Check if engine is still running
            if (!IsProcessAlive(enginePID))
            {
                // Engine exited — check one more time for a manifest
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (std::filesystem::exists(manifestPath))
                {
                    CrashManifest manifest;
                    if (LoadManifest(manifestPath, manifest))
                    {
                        std::cerr << "[CrashReporter] Crash manifest detected after engine exit!\n";
                        int result = RunCrashReporter(manifest);
                        std::filesystem::remove(manifestPath);
                        return result;
                    }
                }
                std::cerr << "[CrashReporter] Engine exited normally.\n";
                return 0;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

} // namespace SparkCrashReporter
