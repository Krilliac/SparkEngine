#include "GitRunner.h"

#include "ProcessRunner.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

namespace SparkInstaller
{
    namespace
    {
        std::string QuoteIfNeeded(const std::string& s)
        {
            if (s.empty())
                return "\"\"";
            bool needsQuotes = s.find(' ') != std::string::npos || s.find('\t') != std::string::npos;
            if (!needsQuotes)
                return s;
            return "\"" + s + "\"";
        }
    } // namespace

    int GitRunner::Run(const std::string& args, const std::string& cwd, const LogSink& log) const
    {
        SparkBuild::ProcessRunner runner;
        std::string cmd = QuoteIfNeeded(m_gitExe) + " " + args;
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
        std::string args = "clone --recurse-submodules --progress";
        if (!ref.empty())
            args += " --branch " + QuoteIfNeeded(ref);
        args += " " + QuoteIfNeeded(repoUrl) + " " + QuoteIfNeeded(destination);
        return Run(args, {}, log) == 0;
    }

    bool GitRunner::Fetch(const std::string& destination, const LogSink& log) const
    {
        return Run("fetch --all --tags --prune", destination, log) == 0;
    }

    bool GitRunner::CheckoutRef(const std::string& ref, const std::string& destination, const LogSink& log) const
    {
        if (Run("checkout " + QuoteIfNeeded(ref), destination, log) != 0)
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
        std::string cmd = QuoteIfNeeded(m_gitExe) + " rev-parse HEAD";
        std::string output;
        if (runner.RunSync(cmd, destination, output) != 0)
            return {};
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
            output.pop_back();
        return output;
    }
} // namespace SparkInstaller
