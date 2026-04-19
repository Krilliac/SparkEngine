#include "GitRunner.h"

#include "Platform.h"
#include "ProcessRunner.h"

#include <cstdio>

namespace SparkInstaller
{
    namespace
    {
#ifdef SPARK_PLATFORM_WINDOWS
        // Windows cmd.exe quoting: wrap in double quotes, escape embedded " and \.
        // The command is run via cmd /c, so we also escape cmd metacharacters.
        std::string ShellQuote(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 4);
            out += '"';
            int backslashes = 0;
            for (char c : s)
            {
                if (c == '\\')
                {
                    ++backslashes;
                    out += c;
                    continue;
                }
                if (c == '"')
                {
                    out.append(static_cast<size_t>(backslashes) + 1, '\\');
                    out += '"';
                }
                else
                {
                    out += c;
                }
                backslashes = 0;
            }
            out.append(static_cast<size_t>(backslashes), '\\');
            out += '"';
            return out;
        }
#else
        // POSIX shell quoting: always single-quote and escape embedded single-quotes via '\''.
        std::string ShellQuote(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 2);
            out += '\'';
            for (char c : s)
            {
                if (c == '\'')
                    out += "'\\''";
                else
                    out += c;
            }
            out += '\'';
            return out;
        }
#endif

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

        // Whitelist-validate a repository URL. Accepts https://, http://, git@host:..., or ssh://.
        // Rejects shell metacharacters anywhere.
        bool IsSafeRepoUrl(const std::string& url)
        {
            if (url.empty() || url.size() > 1024)
                return false;
            for (char c : url)
            {
                if (c == '`' || c == '$' || c == ';' || c == '&' || c == '|' || c == '\n' || c == '\r' || c == '<' ||
                    c == '>' || c == '"' || c == '\'' || c == '\\' || c == ' ' || c == '\t')
                    return false;
            }
            return true;
        }
    } // namespace

    int GitRunner::Run(const std::string& args, const std::string& cwd, const LogSink& log) const
    {
        SparkBuild::ProcessRunner runner;
        std::string cmd = ShellQuote(m_gitExe) + " " + args;
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

        std::string args = "clone --recurse-submodules --progress";
        if (!ref.empty())
            args += " --branch " + ShellQuote(ref);
        args += " " + ShellQuote(repoUrl) + " " + ShellQuote(destination);
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
        if (Run("checkout " + ShellQuote(ref), destination, log) != 0)
            return false;
        Run("pull --ff-only", destination, log);
        return true;
    }

    bool GitRunner::UpdateSubmodules(const std::string& destination, const LogSink& log) const
    {
        return Run("submodule update --init --recursive", destination, log) == 0;
    }

    std::string GitRunner::HeadCommit(const std::string& destination) const
    {
        SparkBuild::ProcessRunner runner;
        std::string cmd = ShellQuote(m_gitExe) + " rev-parse HEAD";
        std::string output;
        if (runner.RunSync(cmd, destination, output) != 0)
            return {};
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
            output.pop_back();
        return output;
    }
} // namespace SparkInstaller
