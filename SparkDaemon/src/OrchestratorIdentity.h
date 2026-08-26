/**
 * @file OrchestratorIdentity.h
 * @brief Locked persistent mutation identity for the SparkOrchestrator CLI.
 */

#pragma once

#include "OrchestrationProtocol.h"

#include <filesystem>
#include <optional>
#include <string>

namespace Spark::Daemon
{
    /**
     * @brief Owns an exclusive lock on a persisted CLI mutation sequence.
     *
     * The lock remains held until destruction so concurrent CLI processes
     * cannot deliver sequence N+1 before the process holding sequence N.
     */
    class OrchestratorIdentityLease final
    {
      public:
        ~OrchestratorIdentityLease();

        OrchestratorIdentityLease(OrchestratorIdentityLease&& other) noexcept;
        OrchestratorIdentityLease& operator=(OrchestratorIdentityLease&& other) noexcept;
        OrchestratorIdentityLease(const OrchestratorIdentityLease&) = delete;
        OrchestratorIdentityLease& operator=(const OrchestratorIdentityLease&) = delete;

        [[nodiscard]] static std::optional<OrchestratorIdentityLease> Acquire(const std::filesystem::path& path,
                                                                              std::string& error);
        [[nodiscard]] const MutationKey& Key() const noexcept { return m_key; }

      private:
        OrchestratorIdentityLease() = default;
        void Release() noexcept;

        MutationKey m_key;
        std::intptr_t m_file = -1;
    };

    [[nodiscard]] std::filesystem::path DefaultOrchestratorIdentityPath();
} // namespace Spark::Daemon
