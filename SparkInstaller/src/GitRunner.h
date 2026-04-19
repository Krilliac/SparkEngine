#pragma once

#include "InstallerContext.h"

#include <string>

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

      private:
        int Run(const std::string& args, const std::string& cwd, const LogSink& log) const;

        std::string m_gitExe;
    };
} // namespace SparkInstaller
