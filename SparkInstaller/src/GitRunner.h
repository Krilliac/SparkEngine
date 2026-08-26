#pragma once

#include "InstallerContext.h"

#include <string>
#include <string_view>

namespace SparkInstaller
{
    class GitRunner
    {
      public:
        explicit GitRunner(std::string gitExe) : m_gitExe(std::move(gitExe)) {}

        bool Clone(const std::string& repoUrl, const std::string& ref, const std::string& destination,
                   const LogSink& log) const;

        bool Fetch(const std::string& destination, const LogSink& log) const;
        bool CheckoutRef(const std::string& ref, const std::string& destination, const LogSink& log) const;
        bool UpdateSubmodules(const std::string& destination, const LogSink& log) const;

        // Resolve the current HEAD commit SHA.
        std::string HeadCommit(const std::string& destination) const;

        /**
         * Encode one argv element for SparkBuild::ProcessRunner::RunSync.
         * RunSync is deliberately shell-less: it recognizes double quotes and
         * backslash escapes, so shell-specific single quoting is incorrect.
         */
        static std::string EncodeProcessRunnerArgument(std::string_view argument);

      private:
        int Run(const std::string& args, const std::string& cwd, const LogSink& log) const;

        std::string m_gitExe;
    };
} // namespace SparkInstaller
