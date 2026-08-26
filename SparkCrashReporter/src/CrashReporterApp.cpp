/**
 * @file CrashReporterApp.cpp
 * @brief Out-of-process crash reporter implementation
 */

#include "CrashReporterApp.h"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

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
    // JSON helpers (bounded, strict, and dependency-free)
    // ============================================================================

    namespace
    {
        constexpr size_t kMaxManifestBytes = 1024 * 1024;
        constexpr size_t kMaxJsonStringBytes = 256 * 1024;
        constexpr size_t kMaxJsonDepth = 16;
        constexpr size_t kMaxCollectionEntries = 4096;

        class ManifestJsonReader
        {
          public:
            explicit ManifestJsonReader(std::string_view json) : m_json(json) {}

            bool Parse(CrashManifest& manifest)
            {
                SkipWhitespace();
                if (!Consume('{'))
                    return false;

                SkipWhitespace();
                if (Consume('}'))
                    return Finish();

                size_t entryCount = 0;
                while (entryCount++ < kMaxCollectionEntries)
                {
                    std::string key;
                    if (!ParseString(key) || !m_seenKeys.insert(key).second)
                        return false;

                    SkipWhitespace();
                    if (!Consume(':'))
                        return false;
                    SkipWhitespace();

                    if (!ParseManifestMember(key, manifest))
                        return false;

                    SkipWhitespace();
                    if (Consume('}'))
                        return Finish();
                    if (!Consume(','))
                        return false;
                    SkipWhitespace();
                }
                return false;
            }

          private:
            bool Finish()
            {
                SkipWhitespace();
                return m_position == m_json.size();
            }

            void SkipWhitespace()
            {
                while (m_position < m_json.size())
                {
                    const char c = m_json[m_position];
                    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                        break;
                    ++m_position;
                }
            }

            bool Consume(char expected)
            {
                if (m_position >= m_json.size() || m_json[m_position] != expected)
                    return false;
                ++m_position;
                return true;
            }

            static int HexValue(char c)
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            }

            bool ParseHex4(uint32_t& value)
            {
                if (m_json.size() - m_position < 4)
                    return false;
                value = 0;
                for (int index = 0; index < 4; ++index)
                {
                    const int digit = HexValue(m_json[m_position++]);
                    if (digit < 0)
                        return false;
                    value = (value << 4) | static_cast<uint32_t>(digit);
                }
                return true;
            }

            static void AppendUtf8(uint32_t codePoint, std::string& output)
            {
                if (codePoint <= 0x7F)
                {
                    output.push_back(static_cast<char>(codePoint));
                }
                else if (codePoint <= 0x7FF)
                {
                    output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                else if (codePoint <= 0xFFFF)
                {
                    output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                else
                {
                    output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                    output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
            }

            bool ParseString(std::string& output)
            {
                if (!Consume('"'))
                    return false;

                output.clear();
                while (m_position < m_json.size())
                {
                    const unsigned char c = static_cast<unsigned char>(m_json[m_position++]);
                    if (c == '"')
                        return output.size() <= kMaxJsonStringBytes;
                    if (c < 0x20)
                        return false;

                    if (c != '\\')
                    {
                        output.push_back(static_cast<char>(c));
                    }
                    else
                    {
                        if (m_position >= m_json.size())
                            return false;
                        const char escape = m_json[m_position++];
                        switch (escape)
                        {
                        case '"':
                        case '\\':
                        case '/':
                            output.push_back(escape);
                            break;
                        case 'b':
                            output.push_back('\b');
                            break;
                        case 'f':
                            output.push_back('\f');
                            break;
                        case 'n':
                            output.push_back('\n');
                            break;
                        case 'r':
                            output.push_back('\r');
                            break;
                        case 't':
                            output.push_back('\t');
                            break;
                        case 'u':
                        {
                            uint32_t codePoint = 0;
                            if (!ParseHex4(codePoint))
                                return false;
                            if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
                            {
                                if (m_json.size() - m_position < 6 || m_json[m_position] != '\\' ||
                                    m_json[m_position + 1] != 'u')
                                    return false;
                                m_position += 2;
                                uint32_t low = 0;
                                if (!ParseHex4(low) || low < 0xDC00 || low > 0xDFFF)
                                    return false;
                                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                            }
                            else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
                            {
                                return false;
                            }
                            AppendUtf8(codePoint, output);
                            break;
                        }
                        default:
                            return false;
                        }
                    }

                    if (output.size() > kMaxJsonStringBytes)
                        return false;
                }
                return false;
            }

            bool ParseBoolean(bool& output)
            {
                if (m_json.substr(m_position, 4) == "true")
                {
                    m_position += 4;
                    output = true;
                    return true;
                }
                if (m_json.substr(m_position, 5) == "false")
                {
                    m_position += 5;
                    output = false;
                    return true;
                }
                return false;
            }

            bool ParseInteger(int& output)
            {
                const size_t start = m_position;
                if (m_position < m_json.size() && m_json[m_position] == '-')
                    ++m_position;
                if (m_position >= m_json.size())
                    return false;
                if (m_json[m_position] == '0')
                {
                    ++m_position;
                    if (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        return false;
                }
                else
                {
                    if (m_json[m_position] < '1' || m_json[m_position] > '9')
                        return false;
                    while (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        ++m_position;
                }

                int64_t parsed = 0;
                const char* begin = m_json.data() + start;
                const char* end = m_json.data() + m_position;
                const auto result = std::from_chars(begin, end, parsed);
                if (result.ec != std::errc{} || result.ptr != end || parsed < std::numeric_limits<int>::min() ||
                    parsed > std::numeric_limits<int>::max())
                    return false;
                output = static_cast<int>(parsed);
                return true;
            }

            bool SkipNumber()
            {
                if (m_position < m_json.size() && m_json[m_position] == '-')
                    ++m_position;
                if (m_position >= m_json.size())
                    return false;
                if (m_json[m_position] == '0')
                {
                    ++m_position;
                }
                else
                {
                    if (m_json[m_position] < '1' || m_json[m_position] > '9')
                        return false;
                    while (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        ++m_position;
                }
                if (m_position < m_json.size() && m_json[m_position] == '.')
                {
                    ++m_position;
                    const size_t fractionStart = m_position;
                    while (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        ++m_position;
                    if (fractionStart == m_position)
                        return false;
                }
                if (m_position < m_json.size() && (m_json[m_position] == 'e' || m_json[m_position] == 'E'))
                {
                    ++m_position;
                    if (m_position < m_json.size() && (m_json[m_position] == '+' || m_json[m_position] == '-'))
                        ++m_position;
                    const size_t exponentStart = m_position;
                    while (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        ++m_position;
                    if (exponentStart == m_position)
                        return false;
                }
                return true;
            }

            bool SkipValue(size_t depth)
            {
                if (depth > kMaxJsonDepth || m_position >= m_json.size())
                    return false;

                if (m_json[m_position] == '"')
                {
                    std::string ignored;
                    return ParseString(ignored);
                }
                if (m_json[m_position] == '{')
                {
                    ++m_position;
                    SkipWhitespace();
                    if (Consume('}'))
                        return true;
                    size_t entries = 0;
                    while (entries++ < kMaxCollectionEntries)
                    {
                        std::string ignoredKey;
                        if (!ParseString(ignoredKey))
                            return false;
                        SkipWhitespace();
                        if (!Consume(':'))
                            return false;
                        SkipWhitespace();
                        if (!SkipValue(depth + 1))
                            return false;
                        SkipWhitespace();
                        if (Consume('}'))
                            return true;
                        if (!Consume(','))
                            return false;
                        SkipWhitespace();
                    }
                    return false;
                }
                if (m_json[m_position] == '[')
                {
                    ++m_position;
                    SkipWhitespace();
                    if (Consume(']'))
                        return true;
                    size_t entries = 0;
                    while (entries++ < kMaxCollectionEntries)
                    {
                        if (!SkipValue(depth + 1))
                            return false;
                        SkipWhitespace();
                        if (Consume(']'))
                            return true;
                        if (!Consume(','))
                            return false;
                        SkipWhitespace();
                    }
                    return false;
                }
                if (m_json.substr(m_position, 4) == "true" || m_json.substr(m_position, 4) == "null")
                {
                    m_position += 4;
                    return true;
                }
                if (m_json.substr(m_position, 5) == "false")
                {
                    m_position += 5;
                    return true;
                }
                return SkipNumber();
            }

            bool ParseManifestMember(const std::string& key, CrashManifest& manifest)
            {
                if (key == "enginePID")
                    return ParseString(manifest.enginePID);
                if (key == "timestamp")
                    return ParseString(manifest.timestamp);
                if (key == "dumpFile")
                    return ParseString(manifest.dumpFile);
                if (key == "logFile")
                    return ParseString(manifest.logFile);
                if (key == "screenshotFile")
                    return ParseString(manifest.screenshotFile);
                if (key == "zipFile")
                    return ParseString(manifest.zipFile);
                if (key == "crashTitle")
                    return ParseString(manifest.crashTitle);
                if (key == "uploadURL")
                    return ParseString(manifest.uploadURL);
                if (key == "proxyURL")
                    return ParseString(manifest.proxyURL);
                if (key == "githubRepo")
                    return ParseString(manifest.githubRepo);
                if (key == "githubToken")
                    return ParseString(manifest.githubToken);
                if (key == "githubLabels")
                    return ParseString(manifest.githubLabels);
                if (key == "smtpUser")
                    return ParseString(manifest.smtpUser);
                if (key == "smtpPass")
                    return ParseString(manifest.smtpPass);
                if (key == "emailTo")
                    return ParseString(manifest.emailTo);
                if (key == "emailFrom")
                    return ParseString(manifest.emailFrom);
                if (key == "requireConsent")
                    return ParseBoolean(manifest.requireConsent);
                if (key == "allowScreenshotRefusal")
                    return ParseBoolean(manifest.allowScreenshotRefusal);
                if (key == "promptUserDescription")
                    return ParseBoolean(manifest.promptUserDescription);
                if (key == "timeoutSeconds")
                    return ParseInteger(manifest.timeoutSeconds);
                return SkipValue(0);
            }

            std::string_view m_json;
            size_t m_position = 0;
            std::unordered_set<std::string> m_seenKeys;
        };
    } // namespace

    static std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 16);
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char c : s)
        {
            if (c == '"')
                out += "\\\"";
            else if (c == '\\')
                out += "\\\\";
            else if (c == '\b')
                out += "\\b";
            else if (c == '\f')
                out += "\\f";
            else if (c == '\n')
                out += "\\n";
            else if (c == '\r')
                out += "\\r";
            else if (c == '\t')
                out += "\\t";
            else if (c < 0x20)
            {
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0F]);
            }
            else
                out += static_cast<char>(c);
        }
        return out;
    }

    // ============================================================================
    // Manifest I/O
    // ============================================================================

    bool LoadManifest(const std::string& path, CrashManifest& out)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open())
            return false;

        const std::streamoff length = static_cast<std::streamoff>(f.tellg());
        if (length <= 0 || static_cast<uint64_t>(length) > kMaxManifestBytes)
            return false;

        std::string json(static_cast<size_t>(length), '\0');
        f.seekg(0, std::ios::beg);
        if (!f.read(json.data(), static_cast<std::streamsize>(json.size())))
            return false;

        // Detect a file that grew after the bounded size check.
        char extra = 0;
        if (f.get(extra))
            return false;

        CrashManifest parsed;
        ManifestJsonReader reader(json);
        if (!reader.Parse(parsed) || parsed.logFile.empty())
            return false;

        out = std::move(parsed);
        return true;
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
        f << "  \"githubLabels\": \"" << JsonEscape(m.githubLabels) << "\",\n";
        f << "  \"smtpUser\": \"" << JsonEscape(m.smtpUser) << "\",\n";
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

        // Preserve the augmented report locally. This standalone executable does
        // not currently implement a network uploader, so never claim it sent data.
        if (!userDescription.empty())
        {
            std::ofstream f(manifest.logFile);
            if (f.is_open())
                f << fullLog;
        }

        std::cerr << "\nCrash report saved locally. Automatic upload is not available in this build.\n";
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
