#include <chrono>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#elif defined(__APPLE__)
#include <libproc.h>
#endif
#endif

namespace
{
    struct Plan
    {
        std::string name = "runtime";
        std::filesystem::path executable;
        std::filesystem::path workingDirectory = ".";
        std::filesystem::path capturedLog = "spark-automation.log";
        std::filesystem::path jsonReport;
        std::filesystem::path junitReport;
        std::vector<std::filesystem::path> screenshots;
        std::vector<std::string> logContains;
        std::vector<std::string> arguments;
        uint32_t frames = 0;
        uint32_t timeoutMs = 30'000;
        int expectedExit = 0;
    };

    struct ProcessResult
    {
        bool launched = false;
        bool timedOut = false;
        int exitCode = -1;
        uint64_t durationMs = 0;
        std::string error;
    };

    template <typename Integer> bool ParseInteger(std::string_view input, Integer& output, bool allowNegative)
    {
        if (input.empty() || input.front() == '+' || (!allowNegative && input.front() == '-'))
            return false;

        Integer parsed{};
        const char* const first = input.data();
        const char* const last = first + input.size();
        const auto [end, error] = std::from_chars(first, last, parsed, 10);
        if (error != std::errc{} || end != last)
            return false;
        output = parsed;
        return true;
    }

    std::string Escape(std::string_view input, bool xml)
    {
        std::string output;
        for (const char value : input)
        {
            if (xml)
            {
                if (value == '&')
                    output += "&amp;";
                else if (value == '<')
                    output += "&lt;";
                else if (value == '>')
                    output += "&gt;";
                else if (value == '"')
                    output += "&quot;";
                else if (value == '\n')
                    output += "&#10;";
                else if (value == '\r')
                    output += "&#13;";
                else if (value == '\t')
                    output += "&#9;";
                else
                    output.push_back(value);
            }
            else
            {
                if (value == '\n')
                    output += "\\n";
                else if (value == '\r')
                    output += "\\r";
                else if (value == '\t')
                    output += "\\t";
                else if (value == '\b')
                    output += "\\b";
                else if (value == '\f')
                    output += "\\f";
                else if (value == '\\' || value == '"')
                {
                    output.push_back('\\');
                    output.push_back(value);
                }
                else if (static_cast<unsigned char>(value) < 0x20)
                {
                    constexpr char kHex[] = "0123456789abcdef";
                    output += "\\u00";
                    output.push_back(kHex[(static_cast<unsigned char>(value) >> 4u) & 0x0fu]);
                    output.push_back(kHex[static_cast<unsigned char>(value) & 0x0fu]);
                }
                else
                    output.push_back(value);
            }
        }
        return output;
    }

#ifdef _WIN32
    std::string QuoteArgument(const std::string& argument)
    {
        if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos)
            return argument;
        std::string result = "\"";
        size_t slashes = 0;
        for (const char value : argument)
        {
            if (value == '\\')
            {
                ++slashes;
            }
            else if (value == '"')
            {
                result.append(slashes * 2 + 1, '\\');
                result.push_back(value);
                slashes = 0;
            }
            else
            {
                result.append(slashes, '\\');
                result.push_back(value);
                slashes = 0;
            }
        }
        result.append(slashes * 2, '\\');
        result.push_back('"');
        return result;
    }
#endif

#ifndef _WIN32
    struct ProcessEntry
    {
        pid_t pid = 0;
        pid_t parent = 0;
    };

    bool ReadProcessTable(std::vector<ProcessEntry>& processes, std::string& error)
    {
        processes.clear();
#ifdef __linux__
        std::error_code iteratorError;
        for (std::filesystem::directory_iterator entry("/proc", iteratorError), end; entry != end;
             entry.increment(iteratorError))
        {
            if (iteratorError)
                break;
            const std::string name = entry->path().filename().string();
            pid_t pid = 0;
            const auto [parsed, parseError] = std::from_chars(name.data(), name.data() + name.size(), pid, 10);
            if (parseError != std::errc{} || parsed != name.data() + name.size() || pid <= 0)
                continue;

            std::ifstream stat(entry->path() / "stat");
            std::string line;
            if (!std::getline(stat, line))
                continue;
            const size_t commandEnd = line.rfind(')');
            if (commandEnd == std::string::npos || commandEnd + 2 >= line.size())
                continue;
            std::istringstream fields(line.substr(commandEnd + 2));
            char state = '\0';
            pid_t parent = 0;
            if (fields >> state >> parent)
                processes.push_back({pid, parent});
        }
        if (iteratorError)
        {
            error = "failed to enumerate /proc while containing timed-out process tree: " + iteratorError.message();
            return false;
        }
        return true;
#elif defined(__APPLE__)
        const int requiredBytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
        if (requiredBytes <= 0)
        {
            error = "failed to size the macOS process table while containing timed-out process tree";
            return false;
        }
        std::vector<pid_t> pids(static_cast<size_t>(requiredBytes) / sizeof(pid_t) + 64u);
        const int populatedBytes =
            proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
        if (populatedBytes < 0)
        {
            error = "failed to enumerate the macOS process table while containing timed-out process tree";
            return false;
        }
        pids.resize(static_cast<size_t>(populatedBytes) / sizeof(pid_t));
        for (const pid_t pid : pids)
        {
            if (pid <= 0)
                continue;
            proc_bsdinfo info{};
            if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == static_cast<int>(sizeof(info)))
                processes.push_back({pid, static_cast<pid_t>(info.pbi_ppid)});
        }
        return true;
#else
        error = "process-tree containment is not implemented on this POSIX platform";
        return false;
#endif
    }

    bool KillProcessTree(pid_t root, std::string& error)
    {
        // Freeze the original process group first. Descendants that created their own
        // session/group are discovered below and frozen individually before anything
        // is killed, preserving their ancestry long enough to contain them reliably.
        (void)kill(-root, SIGSTOP);
        (void)kill(root, SIGSTOP);

        std::unordered_set<pid_t> victims{root};
        unsigned stableScans = 0;
        for (unsigned scan = 0; scan < 20 && stableScans < 2; ++scan)
        {
            std::vector<ProcessEntry> processes;
            if (!ReadProcessTable(processes, error))
                break;

            bool addedAny = false;
            bool addedPass = true;
            while (addedPass)
            {
                addedPass = false;
                for (const ProcessEntry& process : processes)
                {
                    // Linux subreapers adopt descendants whose intermediate parent
                    // already exited, so direct children of this supervisor also
                    // belong to its otherwise single-child containment tree.
                    const bool isDescendant = victims.contains(process.parent) || process.parent == getpid();
                    if (process.pid == getpid() || !isDescendant)
                        continue;
                    if (victims.insert(process.pid).second)
                    {
                        (void)kill(process.pid, SIGSTOP);
                        addedAny = true;
                        addedPass = true;
                    }
                }
            }
            stableScans = addedAny ? 0u : stableScans + 1u;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // Kill individually discovered descendants in addition to the original
        // process group; session leaders are intentionally absent from that group.
        for (const pid_t victim : victims)
        {
            if (victim != root && kill(victim, SIGKILL) != 0 && errno != ESRCH && error.empty())
                error = "failed to kill escaped descendant " + std::to_string(victim) + ": " + std::strerror(errno);
        }
        (void)kill(-root, SIGKILL);
        if (kill(root, SIGKILL) != 0 && errno != ESRCH && error.empty())
            error = "failed to kill timed-out process: " + std::string(std::strerror(errno));
        return error.empty();
    }
#endif

    ProcessResult RunProcess(const Plan& plan)
    {
        ProcessResult result;
        const auto start = std::chrono::steady_clock::now();
        std::error_code ec;
        if (!plan.capturedLog.parent_path().empty())
            std::filesystem::create_directories(plan.capturedLog.parent_path(), ec);
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE log = CreateFileW(plan.capturedLog.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE)
        {
            result.error = "failed to create captured log";
            return result;
        }
        std::string command = QuoteArgument(plan.executable.string());
        for (const auto& argument : plan.arguments)
            command += " " + QuoteArgument(argument);
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.hStdOutput = log;
        startup.hStdError = log;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        const BOOL created =
            CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                           nullptr, plan.workingDirectory.string().c_str(), &startup, &process);
        CloseHandle(log);
        if (!created)
        {
            result.error = "failed to launch process (Windows error " + std::to_string(GetLastError()) + ")";
            return result;
        }
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, process.hProcess))
        {
            result.error = "failed to contain process tree in a Windows job";
            TerminateProcess(process.hProcess, 125);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            if (job)
                CloseHandle(job);
            return result;
        }
        ResumeThread(process.hThread);
        result.launched = true;
        const DWORD wait = WaitForSingleObject(process.hProcess, plan.timeoutMs);
        if (wait == WAIT_TIMEOUT)
        {
            result.timedOut = true;
            TerminateJobObject(job, 124);
            WaitForSingleObject(process.hProcess, 5000);
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        result.exitCode = static_cast<int>(exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
#else
        const int log = open(plan.capturedLog.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (log < 0)
        {
            result.error = "failed to create captured log";
            return result;
        }
#ifdef __linux__
        // Adopt orphaned grandchildren so a timed-out process cannot evade cleanup
        // by briefly double-forking while the process tree is being frozen.
        if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
        {
            result.error = "failed to enable Linux child-subreaper containment: " + std::string(std::strerror(errno));
            close(log);
            return result;
        }
#endif
        const pid_t child = fork();
        if (child == 0)
        {
            setpgid(0, 0);
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            close(log);
            if (chdir(plan.workingDirectory.c_str()) != 0)
                _exit(127);
            std::vector<std::string> values{plan.executable.string()};
            values.insert(values.end(), plan.arguments.begin(), plan.arguments.end());
            std::vector<char*> argv;
            for (auto& value : values)
                argv.push_back(value.data());
            argv.push_back(nullptr);
            execv(values.front().c_str(), argv.data());
            _exit(127);
        }
        close(log);
        if (child < 0)
        {
            result.error = "failed to fork process";
            return result;
        }
        result.launched = true;
        (void)setpgid(child, child);
        int status = 0;
        bool reaped = false;
        const auto deadline = start + std::chrono::milliseconds(plan.timeoutMs);
        while (!reaped)
        {
            const pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child)
            {
                reaped = true;
                break;
            }
            if (waited < 0)
            {
                if (errno == EINTR)
                    continue;
                result.error = "waitpid failed while monitoring process: " + std::string(std::strerror(errno));
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                result.timedOut = true;
                (void)KillProcessTree(child, result.error);
                const auto reapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < reapDeadline)
                {
                    const pid_t killed = waitpid(child, &status, WNOHANG);
                    if (killed == child)
                    {
                        reaped = true;
                        break;
                    }
                    if (killed < 0 && errno != EINTR)
                    {
                        result.error =
                            "waitpid failed while reaping timed-out process: " + std::string(std::strerror(errno));
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
#ifdef __linux__
                // Reap any descendants adopted through PR_SET_CHILD_SUBREAPER.
                while (waitpid(-1, nullptr, WNOHANG) > 0)
                {
                }
#endif
                if (!reaped && result.error.empty())
                    result.error = "timed-out process could not be reaped within 5 seconds";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (reaped)
            result.exitCode =
                WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 128);
#endif
        result.durationMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        return result;
    }

    bool WriteFile(const std::filesystem::path& path, const std::string& contents)
    {
        if (path.empty())
            return true;
        std::error_code ec;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        return static_cast<bool>(output);
    }

    std::string DiagnosticTail(const std::string& log)
    {
        constexpr size_t kMaximumDiagnosticBytes = 12'000;
        if (log.size() <= kMaximumDiagnosticBytes)
            return log;
        return log.substr(log.size() - kMaximumDiagnosticBytes);
    }

    void Usage()
    {
        std::cout << "Usage: SparkAutomation --executable <path> [--working-dir <dir>] [--frames N] "
                     "[--timeout-ms N] [--expected-exit N] [--screenshot <path>] [--log-contains <text>] "
                     "[--captured-log <path>] [--json <path>] [--junit <path>] [-- <runtime args>]\n";
    }
} // namespace

int main(int argc, char** argv)
{
    Plan plan;
    bool runtimeArguments = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (runtimeArguments)
        {
            plan.arguments.push_back(argument);
            continue;
        }
        if (argument == "--")
        {
            runtimeArguments = true;
            continue;
        }
        if (argument == "--help" || argument == "-h")
        {
            Usage();
            return 0;
        }
        if (index + 1 >= argc)
        {
            Usage();
            return 2;
        }
        const std::string value = argv[++index];
        try
        {
            if (argument == "--name")
                plan.name = value;
            else if (argument == "--executable")
                plan.executable = value;
            else if (argument == "--working-dir")
                plan.workingDirectory = value;
            else if (argument == "--captured-log")
                plan.capturedLog = value;
            else if (argument == "--json")
                plan.jsonReport = value;
            else if (argument == "--junit")
                plan.junitReport = value;
            else if (argument == "--screenshot")
                plan.screenshots.emplace_back(value);
            else if (argument == "--log-contains")
                plan.logContains.push_back(value);
            else if (argument == "--frames")
            {
                if (!ParseInteger(value, plan.frames, false))
                    throw std::invalid_argument("invalid frame count");
            }
            else if (argument == "--timeout-ms")
            {
                if (!ParseInteger(value, plan.timeoutMs, false))
                    throw std::invalid_argument("invalid timeout");
            }
            else if (argument == "--expected-exit")
            {
                if (!ParseInteger(value, plan.expectedExit, true))
                    throw std::invalid_argument("invalid expected exit code");
            }
            else
                throw std::invalid_argument("unknown argument");
        }
        catch (const std::exception&)
        {
            std::cerr << "Invalid argument: " << argument << " " << value << '\n';
            return 2;
        }
    }
    if (plan.executable.empty() || !std::filesystem::is_regular_file(plan.executable) || plan.timeoutMs == 0)
    {
        Usage();
        return 2;
    }
    plan.executable = std::filesystem::absolute(plan.executable);
    plan.workingDirectory = std::filesystem::absolute(plan.workingDirectory);
    plan.capturedLog = std::filesystem::absolute(plan.capturedLog);
    for (auto& screenshot : plan.screenshots)
    {
        if (screenshot.is_relative())
            screenshot = plan.workingDirectory / screenshot;
        screenshot = screenshot.lexically_normal();
    }
    if (plan.frames > 0)
    {
        plan.arguments.emplace_back("-test-frames");
        plan.arguments.push_back(std::to_string(plan.frames));
    }

    std::vector<std::string> failures;
    for (const auto& screenshot : plan.screenshots)
    {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(screenshot, ec);
        if (ec == std::errc::no_such_file_or_directory)
            ec.clear();
        if (ec)
        {
            failures.push_back("could not inspect pre-existing screenshot: " + screenshot.string());
            continue;
        }
        if (!std::filesystem::exists(status))
            continue;
        if (!std::filesystem::is_regular_file(status))
        {
            failures.push_back("refusing to replace non-regular screenshot path: " + screenshot.string());
            continue;
        }
        if (!std::filesystem::remove(screenshot, ec) || ec)
            failures.push_back("could not remove stale screenshot before launch: " + screenshot.string());
    }

    const ProcessResult process = RunProcess(plan);
    if (!process.launched)
        failures.push_back(process.error);
    else if (!process.error.empty())
        failures.push_back(process.error);
    if (process.timedOut)
        failures.emplace_back("runtime exceeded timeout");
    if (process.launched && process.exitCode != plan.expectedExit)
        failures.push_back("exit code " + std::to_string(process.exitCode) +
                           " != " + std::to_string(plan.expectedExit));

    std::ifstream logFile(plan.capturedLog, std::ios::binary);
    const std::string log((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
    const std::string diagnosticTail = DiagnosticTail(log);
    for (const auto& expected : plan.logContains)
    {
        if (log.find(expected) == std::string::npos)
            failures.push_back("log is missing: " + expected);
    }
    for (const auto& screenshot : plan.screenshots)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(screenshot, ec) || ec || std::filesystem::file_size(screenshot, ec) == 0)
            failures.push_back("screenshot is missing or empty: " + screenshot.string());
    }
    const bool passed = failures.empty();

    std::ostringstream json;
    json << "{\n  \"schemaVersion\": 1,\n  \"name\": \"" << Escape(plan.name, false) << "\",\n"
         << "  \"passed\": " << (passed ? "true" : "false")
         << ",\n  \"timedOut\": " << (process.timedOut ? "true" : "false") << ",\n  \"exitCode\": " << process.exitCode
         << ",\n  \"expectedExit\": " << plan.expectedExit << ",\n  \"frameLimit\": " << plan.frames
         << ",\n  \"durationMs\": " << process.durationMs
         << ",\n  \"capturedLogPath\": \"" << Escape(plan.capturedLog.string(), false) << "\""
         << ",\n  \"capturedLogTruncated\": " << (log.size() > diagnosticTail.size() ? "true" : "false")
         << ",\n  \"capturedLogTail\": \"" << Escape(diagnosticTail, false) << "\""
         << ",\n  \"failures\": [";
    for (size_t index = 0; index < failures.size(); ++index)
        json << (index ? ", " : "") << "\"" << Escape(failures[index], false) << "\"";
    json << "]\n}\n";

    std::ostringstream junit;
    junit << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"SparkAutomation\" tests=\"1\" failures=\""
          << (passed ? 0 : 1) << "\" time=\"" << (static_cast<double>(process.durationMs) / 1000.0)
          << "\">\n  <testcase classname=\"SparkAutomation\" name=\"" << Escape(plan.name, true) << "\">";
    if (!passed)
    {
        std::ostringstream detail;
        for (const auto& failure : failures)
            detail << failure << '\n';
        junit << "\n    <failure message=\"automation expectation failed\">" << Escape(detail.str(), true)
              << "</failure>\n  ";
    }
    if (!diagnosticTail.empty())
        junit << "\n    <system-out>" << Escape(diagnosticTail, true) << "</system-out>\n  ";
    junit << "</testcase>\n</testsuite>\n";

    if (!WriteFile(plan.jsonReport, json.str()) || !WriteFile(plan.junitReport, junit.str()))
    {
        std::cerr << "Failed to write automation report.\n";
        return 3;
    }
    std::cout << json.str();
    if (!passed && !diagnosticTail.empty())
    {
        std::cerr << "--- captured log tail: " << plan.capturedLog.string() << " ---\n"
                  << diagnosticTail;
        if (diagnosticTail.back() != '\n')
            std::cerr << '\n';
    }
    return passed ? 0 : 1;
}
