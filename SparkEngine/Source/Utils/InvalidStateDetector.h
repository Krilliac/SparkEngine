/**
 * @file InvalidStateDetector.h
 * @brief Runtime detection of impossible/contradictory ECS component state combinations
 * @author Spark Engine Team
 * @date 2026
 *
 * Periodically scans ECS entities for cross-component state invariant violations —
 * impossible combinations like "dead entity in combat" or "static body with velocity."
 * Analogous to server-side movement validation in MMOs that detects desync bugs.
 *
 * Usage:
 * @code
 *   Spark::InvalidStateDetector::GetInstance().Initialize();
 *   Spark::InvalidStateDetector::GetInstance().SetWorld(myWorld);
 *   // In main loop:
 *   Spark::InvalidStateDetector::GetInstance().Update(deltaTime);
 *   // At shutdown:
 *   Spark::InvalidStateDetector::GetInstance().Shutdown();
 * @endcode
 *
 * Game modules can register additional rules via AddRule() during initialization.
 *
 * @note Active in Debug and Release builds. Compiled to no-ops in SPARK_SHIPPING.
 * @see FreezeDetector.h, HitchDetector.h, MemoryMonitor.h
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward declaration — avoids pulling ECS headers into every translation unit
class World;

namespace Spark
{

    // =========================================================================
    // Enums & Data Types
    // =========================================================================

    /// Severity of a detected state violation.
    enum class StateViolationSeverity : uint8_t
    {
        Warning, ///< Suspicious but possibly transient (e.g. one-frame desync).
        Error,   ///< Likely bug — state should not persist.
        Critical ///< Definitely wrong — immediate investigation needed.
    };

    /// A single detected state violation.
    struct StateViolation
    {
        std::string ruleName;  ///< Which rule was violated.
        uint32_t entityId = 0; ///< Entity with the invalid state.
        std::string details;   ///< Human-readable description.
        StateViolationSeverity severity = StateViolationSeverity::Error;
    };

    /// Callback signature for a validation rule check.
    /// The rule iterates the World for its target components and appends any violations found.
    using StateCheckFn = std::function<void(::World&, std::vector<StateViolation>&)>;

    /// A registered validation rule.
    struct StateValidationRule
    {
        std::string name;     ///< Unique rule identifier (e.g. "Health.DeadButPositive").
        std::string category; ///< Grouping for console output (e.g. "Health", "Physics").
        StateViolationSeverity severity = StateViolationSeverity::Error;
        bool enabled = true;  ///< Can be toggled at runtime.
        StateCheckFn checkFn; ///< The actual validation logic.
    };

    // =========================================================================
    // Configuration
    // =========================================================================

    /// Configuration for the InvalidStateDetector.
    struct InvalidStateDetectorConfig
    {
        bool enabled = true;               ///< Master enable/disable.
        float checkIntervalSec = 1.0f;     ///< Seconds between full scans (0 = every frame).
        uint32_t maxViolationHistory = 64; ///< Ring buffer capacity for recent violations.
        bool logOnDetection = true;        ///< Log each violation via SPARK_LOG_WARN.
    };

    /// Snapshot of detector status for diagnostics.
    struct InvalidStateDetectorStatus
    {
        uint32_t totalChecks = 0;                     ///< Number of full scans performed.
        uint32_t totalViolations = 0;                 ///< Cumulative violations detected.
        uint32_t activeRules = 0;                     ///< Number of enabled rules.
        uint32_t totalRules = 0;                      ///< Total registered rules.
        std::vector<StateViolation> recentViolations; ///< Last N violations (ring buffer snapshot).
    };

    // =========================================================================
    // InvalidStateDetector
    // =========================================================================

    /**
     * @brief Singleton runtime detector for impossible ECS state combinations.
     *
     * Periodically iterates ECS component views and checks registered validation
     * rules. Each rule targets specific component combinations and reports any
     * violations. Core engine rules are registered automatically; game modules
     * can add domain-specific rules via AddRule().
     */
    class InvalidStateDetector
    {
      public:
        static InvalidStateDetector& GetInstance();

        void Initialize();
        void Update(float dt);
        void Shutdown();

        /// Set the ECS world to scan. Must be called before Update() does anything.
        void SetWorld(::World* world) { m_world = world; }

        // -- Configuration --
        void Configure(const InvalidStateDetectorConfig& config) { m_config = config; }
        [[nodiscard]] const InvalidStateDetectorConfig& GetConfig() const { return m_config; }

        // -- Rule management --
        void AddRule(StateValidationRule rule);
        void SetRuleEnabled(const std::string& name, bool enabled);

        /**
         * @brief Remove a single rule by its unique name.
         * @param name The rule's @c StateValidationRule::name. No-op if not found.
         */
        void RemoveRule(const std::string& name);

        /**
         * @brief Remove every rule belonging to a category.
         *
         * Each rule carries an owning @c category ("RPG", "RTS", "Base", …). A game
         * module MUST call this in its OnUnload for the categories it added, before
         * its DLL is unloaded — otherwise the detector's next Update() invokes rule
         * lambdas whose code lives in freed module memory.
         *
         * @param category The category to purge. No-op if no rule matches.
         */
        void RemoveRulesByCategory(const std::string& category);

        /**
         * @brief Remove all registered rules (including the engine defaults).
         */
        void ClearRules();

        [[nodiscard]] uint32_t GetRuleCount() const { return static_cast<uint32_t>(m_rules.size()); }

        // -- Query --
        [[nodiscard]] InvalidStateDetectorStatus GetStatus() const;
        [[nodiscard]] uint32_t GetViolationCount() const { return m_totalViolations; }

        void RegisterConsoleCommands();

      private:
        InvalidStateDetector() = default;
        ~InvalidStateDetector() = default;
        InvalidStateDetector(const InvalidStateDetector&) = delete;
        InvalidStateDetector& operator=(const InvalidStateDetector&) = delete;

        void RegisterDefaultRules();
        void RunChecks();

        InvalidStateDetectorConfig m_config;
        std::vector<StateValidationRule> m_rules;
        std::vector<StateViolation> m_violations; ///< Ring buffer of recent violations.

        ::World* m_world = nullptr;
        float m_timeSinceLastCheck = 0.0f;
        uint32_t m_totalChecks = 0;
        uint32_t m_totalViolations = 0;
        bool m_initialized = false;
    };

} // namespace Spark

// =========================================================================
// Shipping-safe macro
// =========================================================================

#ifndef SPARK_SHIPPING
#define SPARK_STATE_CHECK_UPDATE(dt) Spark::InvalidStateDetector::GetInstance().Update(dt)
#else
#define SPARK_STATE_CHECK_UPDATE(dt) ((void)0)
#endif
