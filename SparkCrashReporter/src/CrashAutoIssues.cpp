#include "CrashAutoIssues.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#ifndef SPARK_CRASH_REPORTER_VERSION
#define SPARK_CRASH_REPORTER_VERSION "unknown"
#endif

namespace SparkCrashReporter
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr std::string_view kIssuePrefix = "https://github.com/Krilliac/SparkEngine/issues/";
        constexpr auto kTimeout =
#ifdef SPARK_AUTO_ISSUE_TEST_TIMEOUT_MS
            std::chrono::milliseconds(SPARK_AUTO_ISSUE_TEST_TIMEOUT_MS);
#else
            std::chrono::seconds(15);
#endif
        constexpr size_t kMaxOutput = 1024;

        std::string_view SafeCrashClass(std::string_view title)
        {
            // These are fixed titles emitted by CrashHandler. A manifest is
            // untrusted input, so never publish any other title verbatim.
            constexpr std::array known = {
                std::string_view{"Crash Detected"},
                std::string_view{"Assertion Failure"},
                std::string_view{"Unattended Failure (watchdog)"},
                std::string_view{"Crash Detected (thread-stack capture timed out)"},
                std::string_view{"Assertion Failure (thread-stack capture timed out)"},
                std::string_view{"SIGSEGV"},
                std::string_view{"SIGABRT"},
                std::string_view{"SIGFPE"},
                std::string_view{"Crash"},
            };
            const auto match = std::find(known.begin(), known.end(), title);
            return match == known.end() ? "Unknown" : *match;
        }

        fs::path ConfigRoot()
        {
#ifdef _WIN32
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
            if (length < 2 || length > 32768)
                return {};
            std::wstring value(length, L'\0');
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), length) != length - 1)
                return {};
            value.resize(length - 1);
            const fs::path base(value);
#else
            const char* configured = std::getenv("XDG_CONFIG_HOME");
            const char* home = std::getenv("HOME");
            const fs::path base = configured && *configured ? fs::path(configured)
                                  : home && *home           ? fs::path(home) / ".config"
                                                            : fs::path{};
#endif
            if (base.empty() || !base.is_absolute())
                return {};
            return base / "SparkEngine" / "CrashReporter";
        }

        bool HasUnredirectedAncestors(const fs::path& root, bool allowMissing)
        {
            fs::path current = root.root_path();
            if (current.empty())
                return false;
            for (const fs::path& part : root.relative_path())
            {
                current /= part;
                std::error_code error;
                const fs::file_status status = fs::symlink_status(current, error);
                if (error == std::errc::no_such_file_or_directory && allowMissing)
                    continue;
                if (error || fs::is_symlink(status))
                    return false;
#ifdef _WIN32
                const DWORD attributes = GetFileAttributesW(current.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                    return false;
#endif
            }
            return true;
        }

        bool SafeConfigRoot(const fs::path& root, bool create)
        {
            if (root.empty() || !HasUnredirectedAncestors(root, create))
                return false;
            std::error_code error;
            if (create)
                fs::create_directories(root, error);
            if (error || !fs::is_directory(root, error) || error)
                return false;
            // Recheck after creation so a redirected ancestor cannot move the
            // opt-in into the manifest/artifact tree.
            return HasUnredirectedAncestors(root, false);
        }

        bool IsIssueUrl(std::string_view output)
        {
            while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
                output.remove_suffix(1);
            if (!output.starts_with(kIssuePrefix))
                return false;
            const std::string_view number = output.substr(kIssuePrefix.size());
            return !number.empty() && number.front() != '0' &&
                   std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; });
        }

        bool IsInside(const fs::path& parent, const fs::path& child)
        {
            auto left = parent.begin();
            auto right = child.begin();
            for (; left != parent.end(); ++left, ++right)
            {
                if (right == child.end() || *left != *right)
                    return false;
            }
            return true;
        }

        fs::path FindGh(const CrashManifest& manifest)
        {
#ifdef _WIN32
            const DWORD length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
            if (length < 2 || length > 32768)
                return {};
            std::wstring pathValue(length, L'\0');
            if (GetEnvironmentVariableW(L"PATH", pathValue.data(), length) != length - 1)
                return {};
            pathValue.resize(length - 1);
            constexpr wchar_t separator = L';';
            const fs::path executable = L"gh.exe";
#else
            const char* pathEnvironment = std::getenv("PATH");
            if (!pathEnvironment)
                return {};
            const std::string pathValue(pathEnvironment);
            constexpr char separator = ':';
            const fs::path executable = "gh";
#endif
            std::error_code error;
            const fs::path artifactRoot = fs::weakly_canonical(fs::path(manifest.artifactRoot), error);
            if (error)
                return {};
            size_t start = 0;
            while (start <= pathValue.size())
            {
                const size_t end = pathValue.find(separator, start);
                const auto entry = pathValue.substr(start, end == std::string::npos ? end : end - start);
                const fs::path directory(entry);
                if (directory.is_absolute())
                {
                    const fs::path candidate = fs::weakly_canonical(directory / executable, error);
                    if (!error && !IsInside(artifactRoot, candidate) && fs::is_regular_file(candidate, error) && !error)
                    {
#ifndef _WIN32
                        if (access(candidate.c_str(), X_OK) != 0)
                        {
                            if (end == std::string::npos)
                                break;
                            start = end + 1;
                            continue;
                        }
#endif
                        return candidate;
                    }
                    error.clear();
                }
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return {};
        }

        struct CommandResult
        {
            bool finished = false;
            int exitCode = -1;
            std::string output;
            std::string_view failureClass = "not-started";
            unsigned long systemError = 0;
            unsigned long effectiveWaitMilliseconds = 0;
        };

        std::string CommandFailureReason(const CommandResult& command)
        {
            return "GitHub CLI " + std::string(command.failureClass) + " (OS error " +
                   std::to_string(command.systemError) + ", wait limit " +
                   std::to_string(command.effectiveWaitMilliseconds) + " ms); delivery is uncertain";
        }

#ifdef _WIN32
        CommandResult RunGh(const fs::path& executable, const fs::path& cwd, const std::string& body,
                            bool versionProbe = false)
        {
            CommandResult result;
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
            HANDLE readPipe = nullptr;
            HANDLE writePipe = nullptr;
            if (!CreatePipe(&readPipe, &writePipe, &attributes, 0))
            {
                result.failureClass = "pipe-create-failed";
                result.systemError = GetLastError();
                return result;
            }
            if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0))
            {
                result.failureClass = "pipe-isolation-failed";
                result.systemError = GetLastError();
                CloseHandle(readPipe);
                CloseHandle(writePipe);
                return result;
            }
            HANDLE nullHandle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            &attributes, OPEN_EXISTING, 0, nullptr);
            if (nullHandle == INVALID_HANDLE_VALUE)
            {
                result.failureClass = "null-handle-failed";
                result.systemError = GetLastError();
                CloseHandle(readPipe);
                CloseHandle(writePipe);
                return result;
            }
            SIZE_T attributeBytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            std::vector<std::byte> attributeStorage(attributeBytes);
            auto* attributesList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
            HANDLE childHandles[] = {nullHandle, writePipe};
            const bool initialized =
                attributeBytes != 0 && InitializeProcThreadAttributeList(attributesList, 1, 0, &attributeBytes);
            const bool restricted =
                initialized && UpdateProcThreadAttribute(attributesList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                         childHandles, sizeof(childHandles), nullptr, nullptr);
            if (!restricted)
            {
                result.failureClass = "handle-allowlist-failed";
                result.systemError = GetLastError();
                if (initialized)
                    DeleteProcThreadAttributeList(attributesList);
                CloseHandle(readPipe);
                CloseHandle(writePipe);
                CloseHandle(nullHandle);
                return result;
            }
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = nullHandle;
            startup.StartupInfo.hStdOutput = writePipe;
            startup.StartupInfo.hStdError = nullHandle;
            startup.lpAttributeList = attributesList;
            PROCESS_INFORMATION process{};
            // All variable text is generated from fixed literals and a hexadecimal ID.
            std::wstring command = L"\"" + executable.wstring() + L"\"";
            if (versionProbe)
                command += L" --version";
            else
            {
                command += L" issue create --repo github.com/Krilliac/SparkEngine --title "
                           L"\"SparkEngine automatic crash report\" --body \"";
                command.append(body.begin(), body.end());
                command += L"\"";
            }
            const BOOL launched = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                                 CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.c_str(),
                                                 &startup.StartupInfo, &process);
            DeleteProcThreadAttributeList(attributesList);
            CloseHandle(writePipe);
            CloseHandle(nullHandle);
            if (!launched)
            {
                result.failureClass = "process-launch-failed";
                result.systemError = GetLastError();
                CloseHandle(readPipe);
                return result;
            }
            result.effectiveWaitMilliseconds =
                static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(kTimeout).count());
            const DWORD wait = WaitForSingleObject(process.hProcess, result.effectiveWaitMilliseconds);
            if (wait != WAIT_OBJECT_0)
            {
                result.failureClass = wait == WAIT_TIMEOUT ? "process-timeout" : "process-wait-failed";
                result.systemError = wait == WAIT_TIMEOUT ? 0 : GetLastError();
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, INFINITE);
            }
            if (wait == WAIT_OBJECT_0)
            {
                DWORD exitCode = 0;
                if (GetExitCodeProcess(process.hProcess, &exitCode))
                {
                    result.finished = true;
                    result.exitCode = static_cast<int>(exitCode);
                    result.failureClass = "none";
                }
                else
                {
                    result.failureClass = "exit-code-failed";
                    result.systemError = GetLastError();
                }
                DWORD available = 0;
                if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr))
                {
                    const DWORD pipeError = GetLastError();
                    // A gh failure can exit without writing stdout. Once its
                    // write handle closes, an empty anonymous pipe reports a
                    // broken pipe; that is not a failed output inspection.
                    if (pipeError != ERROR_BROKEN_PIPE && pipeError != ERROR_NO_DATA &&
                        pipeError != ERROR_PIPE_NOT_CONNECTED)
                    {
                        result.failureClass = "output-inspection-failed";
                        result.systemError = pipeError;
                        result.finished = false;
                    }
                }
                else if (available > kMaxOutput)
                {
                    result.failureClass = "output-limit-exceeded";
                    result.finished = false;
                }
                else
                {
                    result.output.resize(available);
                    DWORD count = 0;
                    if (available &&
                        (!ReadFile(readPipe, result.output.data(), available, &count, nullptr) || count != available))
                    {
                        result.failureClass = "output-read-failed";
                        result.systemError = GetLastError();
                        result.finished = false;
                        result.output.clear();
                    }
                }
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(readPipe);
            return result;
        }
#else
        bool CloseNonStandardDescriptors()
        {
#if defined(__linux__) && defined(SYS_close_range)
            if (syscall(SYS_close_range, 3U, ~0U, 0U) == 0)
                return true;
#endif
            const long maximum = sysconf(_SC_OPEN_MAX);
            if (maximum < 3 || maximum > 1048576)
                return false; // Fail closed rather than inherit an unknown descriptor set.
            for (int descriptor = 3; descriptor < maximum; ++descriptor)
                close(descriptor);
            return true;
        }

        CommandResult RunGh(const fs::path& executable, const fs::path& cwd, const std::string& body,
                            bool versionProbe = false)
        {
            CommandResult result;
            result.effectiveWaitMilliseconds =
                static_cast<unsigned long>(std::chrono::duration_cast<std::chrono::milliseconds>(kTimeout).count());
            const std::string path = executable.string();
            int pipeEnds[2];
            if (pipe(pipeEnds) != 0)
            {
                result.failureClass = "pipe-create-failed";
                result.systemError = static_cast<unsigned long>(errno);
                return result;
            }
            const pid_t child = fork();
            if (child < 0)
            {
                result.failureClass = "process-launch-failed";
                result.systemError = static_cast<unsigned long>(errno);
                close(pipeEnds[0]);
                close(pipeEnds[1]);
                return result;
            }
            if (child == 0)
            {
                close(pipeEnds[0]);
                const int nullHandle = open("/dev/null", O_RDWR);
                if (nullHandle < 0 || chdir(cwd.c_str()) != 0 || dup2(nullHandle, STDIN_FILENO) < 0 ||
                    dup2(pipeEnds[1], STDOUT_FILENO) < 0 || dup2(nullHandle, STDERR_FILENO) < 0)
                    _exit(127);
                if (!CloseNonStandardDescriptors())
                    _exit(127);
                char* const issueArguments[] = {const_cast<char*>(path.c_str()),
                                                const_cast<char*>("issue"),
                                                const_cast<char*>("create"),
                                                const_cast<char*>("--repo"),
                                                const_cast<char*>("github.com/Krilliac/SparkEngine"),
                                                const_cast<char*>("--title"),
                                                const_cast<char*>("SparkEngine automatic crash report"),
                                                const_cast<char*>("--body"),
                                                const_cast<char*>(body.c_str()),
                                                nullptr};
                char* const versionArguments[] = {const_cast<char*>(path.c_str()), const_cast<char*>("--version"),
                                                  nullptr};
                execv(path.c_str(), versionProbe ? versionArguments : issueArguments);
                _exit(127);
            }
            close(pipeEnds[1]);
            fcntl(pipeEnds[0], F_SETFL, O_NONBLOCK);
            const auto deadline = std::chrono::steady_clock::now() + kTimeout;
            int status = 0;
            bool overflow = false;
            while (true)
            {
                std::array<char, 256> buffer{};
                const ssize_t count = read(pipeEnds[0], buffer.data(), buffer.size());
                if (count > 0)
                {
                    if (result.output.size() + static_cast<size_t>(count) > kMaxOutput)
                        overflow = true;
                    else
                        result.output.append(buffer.data(), static_cast<size_t>(count));
                }
                const pid_t finished = waitpid(child, &status, WNOHANG);
                if (finished == child)
                {
                    result.finished = true;
                    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    result.failureClass = "none";
                    while (true)
                    {
                        const ssize_t extra = read(pipeEnds[0], buffer.data(), buffer.size());
                        if (extra <= 0)
                            break;
                        if (result.output.size() + static_cast<size_t>(extra) > kMaxOutput)
                            overflow = true;
                        else
                            result.output.append(buffer.data(), static_cast<size_t>(extra));
                    }
                    break;
                }
                if (finished < 0 || overflow || std::chrono::steady_clock::now() >= deadline)
                {
                    result.failureClass = finished < 0 ? "process-wait-failed"
                                          : overflow   ? "output-limit-exceeded"
                                                       : "process-timeout";
                    result.systemError = finished < 0 ? static_cast<unsigned long>(errno) : 0;
                    kill(child, SIGKILL);
                    waitpid(child, &status, 0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            close(pipeEnds[0]);
            if (overflow)
                result.finished = false;
            return result;
        }
#endif
    } // namespace

    bool AutoIssuesEnabled()
    {
        const fs::path root = ConfigRoot();
        if (!SafeConfigRoot(root, false))
            return false;
        const fs::path marker = root / "auto-issues-v1.enabled";
        std::error_code error;
        if (!fs::is_regular_file(marker, error) || error || fs::is_symlink(fs::symlink_status(marker, error)) ||
            error || fs::file_size(marker, error) != 11 || error)
            return false;
        std::ifstream input(marker, std::ios::binary);
        std::string value;
        std::getline(input, value);
        return input.good() || input.eof() ? value == "enabled-v1" : false;
    }

    bool SetAutoIssuesEnabled(bool enabled)
    {
        const fs::path root = ConfigRoot();
        if (!enabled && !fs::exists(root))
            return !root.empty();
        if (!SafeConfigRoot(root, enabled))
            return false;
        const fs::path marker = root / "auto-issues-v1.enabled";
        std::error_code error;
        if (!enabled)
        {
            if (!fs::exists(marker, error))
                return !error;
            if (error || fs::is_symlink(fs::symlink_status(marker, error)) || error)
                return false;
            return fs::remove(marker, error) && !error;
        }
        if (fs::exists(marker, error))
            return !error && AutoIssuesEnabled();
        if (error)
            return false;
#ifdef _WIN32
        HANDLE handle = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        constexpr char content[] = "enabled-v1\n";
        DWORD written = 0;
        const bool okay =
            WriteFile(handle, content, sizeof(content) - 1, &written, nullptr) && written == sizeof(content) - 1;
        CloseHandle(handle);
        return okay;
#else
        const int handle = open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (handle < 0)
            return false;
        constexpr char content[] = "enabled-v1\n";
        const bool okay = write(handle, content, sizeof(content) - 1) == sizeof(content) - 1;
        close(handle);
        return okay;
#endif
    }

    std::string CrashReceiptKey(const CrashManifest& manifest)
    {
        const std::string leaf = fs::path(manifest.logFile).filename().string();
        std::uint64_t hash = 14695981039346656037ull;
        const auto add = [&hash](unsigned char byte) { hash = (hash ^ byte) * 1099511628211ull; };
        for (const unsigned char byte : leaf)
            add(byte);
        for (const std::uint64_t number : {manifest.logIdentity.device, manifest.logIdentity.file})
        {
            for (unsigned int index = 0; index < 8; ++index)
                add(static_cast<unsigned char>(number >> (index * 8)));
        }
        std::ostringstream text;
        text << std::hex << std::setfill('0') << std::setw(16) << hash;
        return text.str();
    }

    std::string GeneratePublicIncidentId()
    {
        std::array<unsigned char, 16> bytes{};
#ifdef _WIN32
        if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) !=
            0)
            return {};
#else
        const int handle = open("/dev/urandom", O_RDONLY);
        if (handle < 0)
            return {};
        size_t offset = 0;
        while (offset < bytes.size())
        {
            const ssize_t count = read(handle, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
            {
                close(handle);
                return {};
            }
            offset += static_cast<size_t>(count);
        }
        close(handle);
#endif
        constexpr char hex[] = "0123456789abcdef";
        std::string output;
        output.reserve(bytes.size() * 2);
        for (const unsigned char byte : bytes)
        {
            output.push_back(hex[byte >> 4]);
            output.push_back(hex[byte & 15]);
        }
        return output;
    }

    PreparedAutoIssue PrepareAutoIssue(const CrashManifest& manifest, const std::string& incidentId)
    {
        if (!AutoIssuesEnabled())
            return {false, {}, {}, {}, "automatic GitHub Issues are disabled"};
        if (incidentId.size() != 32 || !std::all_of(incidentId.begin(), incidentId.end(), [](char c)
                                                    { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            return {false, {}, {}, {}, "invalid incident identifier"};
        const fs::path gh = FindGh(manifest);
        if (gh.empty())
            return {false, {}, {}, {}, "GitHub CLI not found on an absolute PATH entry"};
        const fs::path cwd = ConfigRoot();
        if (!SafeConfigRoot(cwd, false))
            return {false, {}, {}, {}, "user-local configuration directory unavailable"};
#ifdef _WIN32
        constexpr std::string_view platform = "Windows";
#else
        constexpr std::string_view platform = "POSIX";
#endif
        const std::string body = "Automatic SparkEngine crash notification.\n\n"
                                 "Reporter version: " SPARK_CRASH_REPORTER_VERSION "\nPlatform: " +
                                 std::string(platform) +
                                 "\nCrash class: " + std::string(SafeCrashClass(manifest.crashTitle)) +
                                 "\nIncident: " + incidentId +
                                 "\n\nNo crash log, dump, screenshot, file path, command line, or personal data "
                                 "was attached. A playtester may add sanitized reproduction steps manually.";
        return {true, gh, cwd, body, {}};
    }

    AutoIssueResult SubmitPreparedAutoIssue(const PreparedAutoIssue& prepared)
    {
        if (!prepared.ready)
            return {false, {}, "automatic issue preflight did not complete"};
        const CommandResult command = RunGh(prepared.ghExecutable, prepared.workingDirectory, prepared.body);
        if (!command.finished)
            return {false, {}, CommandFailureReason(command)};
        if (command.exitCode != 0)
            return {false, {}, "GitHub CLI could not create an issue; check authentication and network access"};
        if (!IsIssueUrl(command.output))
            return {false, {}, "GitHub CLI did not confirm an issue URL; delivery is uncertain"};
        std::string url = command.output;
        while (!url.empty() && (url.back() == '\n' || url.back() == '\r'))
            url.pop_back();
        return {true, std::move(url), {}};
    }

    std::string ProbeGhVersion(const std::filesystem::path& ghExecutable)
    {
        std::error_code error;
        if (!ghExecutable.is_absolute() || !std::filesystem::is_regular_file(ghExecutable, error) || error)
            return "invalid executable path";
        const CommandResult command = RunGh(ghExecutable, std::filesystem::temp_directory_path(), {}, true);
        if (!command.finished)
            return CommandFailureReason(command);
        if (command.exitCode != 0)
            return "GitHub CLI version probe exited " + std::to_string(command.exitCode);
        return "ok";
    }
} // namespace SparkCrashReporter
