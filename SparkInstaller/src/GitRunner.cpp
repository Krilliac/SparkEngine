#include "GitRunner.h"

#include "ProcessRunner.h"

#include <cstdio>

namespace SparkInstaller
{
    namespace
    {
        // Whitelist-validate a git ref (branch or tag). Rejects any input
        // outside [A-Za-z0-9._/+-], which is a conservative subset of what
        // git itself allows and blocks every shell metacharacter.
        bool IsSafeRef(const std::string& ref)
        {
            if (ref.empty() || ref.size() > 255)
                return false;
            for (char c : ref)
            {
                bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '_' || c == '/' || c == '-' || c == '+';
                if (!ok)
                    return false;
            }
            // Git also disallows "..", leading "-", and consecutive slashes.
            if (ref.front() == '-' || ref.front() == '.' || ref.find("..") != std::string::npos)
                return false;
            return true;
        }

        bool StartsWith(std::string_view value, std::string_view prefix)
        {
            return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
        }

        // Accept explicit network URL forms, scp-style git@host:path, and
        // explicit local paths. In particular, never allow an option-shaped
        // repository argument to reach `git clone`.
        bool IsSafeRepoUrl(const std::string& url)
        {
            if (url.empty() || url.size() > 1024 || url.front() == '-')
                return false;
            for (const unsigned char character : url)
            {
                if (character < 32 || character == 127)
                    return false;
            }

            const bool networkUrl = StartsWith(url, "https://") || StartsWith(url, "http://") ||
                                    StartsWith(url, "ssh://") || StartsWith(url, "git://") ||
                                    StartsWith(url, "file://");
            const bool scpUrl = StartsWith(url, "git@") && url.find(':', 4) != std::string::npos;
            const bool posixPath = StartsWith(url, "/") || StartsWith(url, "./") || StartsWith(url, "../");
            const bool uncPath = StartsWith(url, "\\\\");
            const bool drivePath = url.size() >= 3 &&
                                   ((url[0] >= 'A' && url[0] <= 'Z') || (url[0] >= 'a' && url[0] <= 'z')) &&
                                   url[1] == ':' && (url[2] == '/' || url[2] == '\\');
            if ((networkUrl || scpUrl) && url.find(' ') != std::string::npos)
                return false;
            return networkUrl || scpUrl || posixPath || uncPath || drivePath;
        }

        bool IsSafeCloneDestination(const std::string& destination)
        {
            if (destination.empty() || destination.size() > 32767 || destination.front() == '-')
                return false;
            for (const unsigned char character : destination)
            {
                if (character < 32 || character == 127)
                    return false;
            }
            return true;
        }
    } // namespace

    std::string GitRunner::EncodeProcessRunnerArgument(std::string_view argument)
    {
        std::string encoded;
        encoded.reserve(argument.size() * 2 + 2);
        encoded.push_back('"');
        for (const char character : argument)
        {
            if (character == '\\' || character == '"')
                encoded.push_back('\\');
            encoded.push_back(character);
        }
        encoded.push_back('"');
        return encoded;
    }

    int GitRunner::Run(const std::string& args, const std::string& cwd, const LogSink& log) const
    {
        SparkBuild::ProcessRunner runner;
        std::string cmd = EncodeProcessRunnerArgument(m_gitExe) + " " + args;
        if (log)
            log("$ git " + args);

        std::string output;
        int exitCode = runner.RunSync(cmd, cwd, output);
        if (log && !output.empty())
            log(output);
        return exitCode;
    }

    bool GitRunner::Clone(const std::string& repoUrl, const std::string& ref, const std::string& destination,
                          const LogSink& log) const
    {
        if (!IsSafeRepoUrl(repoUrl))
        {
            if (log)
                log("error: repository URL contains unsafe characters: " + repoUrl);
            return false;
        }
        if (!ref.empty() && !IsSafeRef(ref))
        {
            if (log)
                log("error: git ref contains unsafe characters: " + ref);
            return false;
        }
        if (!IsSafeCloneDestination(destination))
        {
            if (log)
                log("error: clone destination is empty, option-shaped, or contains control characters: " + destination);
            return false;
        }

        std::string args = "clone --recurse-submodules --progress";
        if (!ref.empty())
            args += " --branch " + EncodeProcessRunnerArgument(ref);
        args += " -- " + EncodeProcessRunnerArgument(repoUrl) + " " + EncodeProcessRunnerArgument(destination);
        return Run(args, {}, log) == 0;
    }

    bool GitRunner::Fetch(const std::string& destination, const LogSink& log) const
    {
        return Run("fetch --all --tags --prune", destination, log) == 0;
    }

    bool GitRunner::CheckoutRef(const std::string& ref, const std::string& destination, const LogSink& log) const
    {
        if (!IsSafeRef(ref))
        {
            if (log)
                log("error: git ref contains unsafe characters: " + ref);
            return false;
        }
        if (Run("checkout " + EncodeProcessRunnerArgument(ref), destination, log) != 0)
            return false;

        // A detached tag/commit is already exact after fetch + checkout. If a
        // matching origin branch exists, require its explicit fast-forward even
        // when the local branch has no configured upstream; otherwise the
        // installer could build stale local source while reporting success.
        const std::string remoteRef = "refs/remotes/origin/" + ref;
        if (Run("show-ref --verify --quiet " + EncodeProcessRunnerArgument(remoteRef), destination, log) != 0)
            return true;
        return Run("pull --ff-only origin " + EncodeProcessRunnerArgument(ref), destination, log) == 0;
    }

    bool GitRunner::UpdateSubmodules(const std::string& destination, const LogSink& log) const
    {
        return Run("submodule update --init --recursive", destination, log) == 0;
    }

    std::string GitRunner::HeadCommit(const std::string& destination) const
    {
        SparkBuild::ProcessRunner runner;
        std::string cmd = EncodeProcessRunnerArgument(m_gitExe) + " rev-parse HEAD";
        std::string output;
        if (runner.RunSync(cmd, destination, output) != 0)
            return {};
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
            output.pop_back();
        return output;
    }
} // namespace SparkInstaller
