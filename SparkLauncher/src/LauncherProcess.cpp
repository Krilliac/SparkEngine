/**
 * @file LauncherProcess.cpp
 * @brief Cross-platform SparkLauncher child-process construction and launch.
 */

#include "LauncherProcess.h"

#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

namespace SparkLauncher
{
    namespace
    {
        std::filesystem::path ExecutablePath(const std::filesystem::path& directory, const char* name)
        {
#ifdef _WIN32
            return directory / (std::string(name) + ".exe");
#else
            return directory / name;
#endif
        }

#ifdef _WIN32
        void AppendQuotedArgument(std::string& commandLine, const std::string& argument)
        {
            commandLine.push_back('"');
            size_t backslashes = 0;
            for (const char character : argument)
            {
                if (character == '\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == '"')
                {
                    commandLine.append(backslashes * 2 + 1, '\\');
                    commandLine.push_back('"');
                    backslashes = 0;
                    continue;
                }
                commandLine.append(backslashes, '\\');
                backslashes = 0;
                commandLine.push_back(character);
            }
            commandLine.append(backslashes * 2, '\\');
            commandLine.push_back('"');
        }
#endif
    } // namespace

    std::expected<LaunchRequest, std::string> BuildLaunchRequest(const std::filesystem::path& binaryDirectory,
                                                                 const std::filesystem::path& projectFile,
                                                                 LaunchTarget target)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(projectFile, error))
            return std::unexpected("Project file not found: " + projectFile.string());
        if (projectFile.extension() != ".sparkproject")
            return std::unexpected("Expected a .sparkproject file: " + projectFile.string());

        LaunchRequest request;
        request.workingDirectory = projectFile.parent_path();
        switch (target)
        {
        case LaunchTarget::Editor:
            request.executable = ExecutablePath(binaryDirectory, "SparkEditor");
            request.arguments = {"--project", projectFile.string()};
            break;
        case LaunchTarget::Game:
            request.executable = ExecutablePath(binaryDirectory, "SparkEngine");
            request.arguments = {"--project", projectFile.string()};
            break;
        case LaunchTarget::DedicatedServer:
        {
            const std::filesystem::path config = request.workingDirectory / "Config" / "server.ini";
            if (!std::filesystem::is_regular_file(config, error))
                return std::unexpected("Dedicated server config not found: " + config.string());
            request.executable = ExecutablePath(binaryDirectory, "SparkServer");
            request.arguments = {"--config", config.string()};
            break;
        }
        case LaunchTarget::ServiceTopology:
            request.executable = ExecutablePath(binaryDirectory, "SparkEditor");
            request.arguments = {"--project", projectFile.string(), "--open-panel", "ServiceTopology"};
            break;
        }

        if (!std::filesystem::is_regular_file(request.executable, error))
            return std::unexpected(std::string(LaunchTargetName(target)) +
                                   " executable not found: " + request.executable.string());
        return request;
    }

    std::expected<void, std::string> LaunchDetached(const LaunchRequest& request)
    {
#ifdef _WIN32
        std::string commandLine;
        AppendQuotedArgument(commandLine, request.executable.string());
        for (const auto& argument : request.arguments)
        {
            commandLine.push_back(' ');
            AppendQuotedArgument(commandLine, argument);
        }

        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL started =
            CreateProcessA(request.executable.string().c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                           DETACHED_PROCESS, nullptr, request.workingDirectory.string().c_str(), &startup, &process);
        if (!started)
            return std::unexpected("CreateProcess failed (error " + std::to_string(GetLastError()) + ")");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
#else
        const pid_t child = fork();
        if (child < 0)
            return std::unexpected(std::string("fork failed: ") + std::strerror(errno));
        if (child == 0)
        {
            (void)setsid();
            if (!request.workingDirectory.empty() && chdir(request.workingDirectory.c_str()) != 0)
                _exit(126);

            std::vector<char*> arguments;
            std::string executable = request.executable.string();
            arguments.reserve(request.arguments.size() + 2);
            arguments.push_back(executable.data());
            for (const auto& argument : request.arguments)
                arguments.push_back(const_cast<char*>(argument.c_str()));
            arguments.push_back(nullptr);
            execv(executable.c_str(), arguments.data());
            _exit(127);
        }
#endif
        return {};
    }

    const char* LaunchTargetName(LaunchTarget target)
    {
        switch (target)
        {
        case LaunchTarget::Editor:
            return "Editor";
        case LaunchTarget::Game:
            return "Game";
        case LaunchTarget::DedicatedServer:
            return "Dedicated server";
        case LaunchTarget::ServiceTopology:
            return "Service topology";
        }
        return "Spark process";
    }
} // namespace SparkLauncher
