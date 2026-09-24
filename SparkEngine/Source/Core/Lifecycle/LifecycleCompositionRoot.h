/**
 * @file LifecycleCompositionRoot.h
 * @brief Fail-closed owner of the engine lifecycle stages (initialize, update, shutdown)
 *
 * The composition root runs every stage's Initialize in deterministic order.
 * A stage that returns false or throws aborts startup: the root rolls back the
 * stages it already touched (in reverse order), latches a Failed state that
 * later RunUpdate/RunShutdown calls respect, and reports the failure to the
 * caller so the process can exit non-zero.
 */

#pragma once

#include "Core/Lifecycle/LifecycleStage.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Spark::Core::Lifecycle
{
    /// Phase a lifecycle stage takes part in.
    enum class LifecyclePhase : uint8_t
    {
        Initialize,
        Update,
        Shutdown
    };

    /// A stage name that must be present and wired for a phase, or the
    /// composition is rejected before any stage runs.
    struct RequiredStage
    {
        std::string_view name;
        LifecyclePhase phase;
    };

    /// Observable state of a composition root.
    enum class LifecycleRootState : uint8_t
    {
        Idle,         ///< Constructed, nothing initialized yet
        Initializing, ///< RunInitialize in progress
        Initialized,  ///< Every stage initialized successfully
        ShuttingDown, ///< RunShutdown in progress
        ShutDown,     ///< Every teardown ran cleanly; RunInitialize may start again
        Failed        ///< Latched: invalid composition, init failure, or teardown failure
    };

    /**
     * @brief Runs lifecycle stages in (Order, Name) order and fails closed.
     *
     * Rollback and shutdown call Shutdown() in reverse stage order on every stage
     * that supports it and either (a) also supports Initialize and had its
     * Initialize attempted, including the stage that failed (it may be partially
     * initialized), or (b) is a teardown-only stage, which owns teardown of the
     * whole lifecycle and must therefore tolerate partial initialization.
     *
     * Not thread-safe: every method must be called from the main thread, which
     * matches the MainThread affinity of the production stages.
     */
    class LifecycleCompositionRoot
    {
      public:
        /**
         * @brief The process-wide root built from the production engine stages.
         */
        static LifecycleCompositionRoot& Get();

        /**
         * @brief Build a root from explicit stages (tests inject failing stages here).
         * @param stages Stages to own; a null entry makes the composition invalid.
         * @param requiredStages Stage/phase pairs that must be present, or the
         *        composition is invalid and every run fails closed.
         */
        LifecycleCompositionRoot(std::vector<std::unique_ptr<LifecycleStage>> stages,
                                 std::span<const RequiredStage> requiredStages);

        LifecycleCompositionRoot(const LifecycleCompositionRoot&) = delete;
        LifecycleCompositionRoot& operator=(const LifecycleCompositionRoot&) = delete;

        /**
         * @brief Initialize every stage in order, rolling back on the first failure.
         * @return true when all stages initialized (or already were); false when the
         *         composition is invalid, a stage failed, or the root is latched Failed.
         */
        bool RunInitialize();

        /**
         * @brief Update every stage; a no-op once the root is Failed or shut down.
         * @param dt Delta time in seconds.
         */
        void RunUpdate(float dt);

        /**
         * @brief Tear down in reverse stage order, containing stage exceptions.
         * @return true when every teardown ran cleanly (or the root was already
         *         shut down); false when a stage threw or the root is Failed.
         */
        bool RunShutdown();

        /// @return Current lifecycle state.
        LifecycleRootState GetState() const { return m_state; }

        /// @return true when the stage composition passed validation.
        bool IsConfigurationValid() const { return m_isValid; }

      private:
        bool ValidateConfiguration(std::span<const RequiredStage> requiredStages) const;
        bool InitializeStage(LifecycleStage& stage);
        bool RunTeardown();

        std::vector<std::unique_ptr<LifecycleStage>> m_stages;
        std::vector<bool> m_initializeAttempted;
        LifecycleRootState m_state = LifecycleRootState::Idle;
        bool m_isValid = false;
    };
} // namespace Spark::Core::Lifecycle
