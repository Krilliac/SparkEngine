/**
 * @file ScriptSandbox.h
 * @brief Runtime sandboxing for AngelScript execution
 *
 * Enforces resource limits (instruction count, execution time) and API access
 * control on AngelScript contexts. Prevents untrusted scripts from freezing the
 * engine or calling restricted functions.
 *
 * Three security levels:
 * - Unrestricted: no limits (editor/dev mode)
 * - Standard: instruction + time limits, full API (shipping default)
 * - Strict: whitelist-only API, tight limits (untrusted mods/UGC)
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef SPARK_ANGELSCRIPT_SUPPORT
class asIScriptContext; // angelscript.h declares it as a class (C4099 otherwise)
#endif

namespace Spark
{

    /**
     * @brief Security level for script execution
     */
    enum class ScriptSecurityLevel : uint8_t
    {
        Unrestricted, ///< No limits — editor and development mode
        Standard,     ///< Instruction + time limits, full API access
        Strict        ///< Whitelist-only API, tight resource limits
    };

    /**
     * @brief Types of sandbox violations
     */
    enum class SandboxViolationType : uint8_t
    {
        InstructionLimit, ///< Script exceeded max instruction count
        TimeoutLimit,     ///< Script exceeded max execution time
        BlockedFunction   ///< Script attempted to call a blocked function
    };

    /**
     * @brief Record of a single sandbox violation
     */
    struct SandboxViolation
    {
        SandboxViolationType type;
        std::string scriptName; ///< Module or class that violated
        std::string details;    ///< Human-readable description
        uint64_t timestamp = 0; ///< Frame number when violation occurred
    };

    /**
     * @brief Runtime sandbox for AngelScript execution
     *
     * Enforces instruction limits, execution timeouts, and API access control.
     * Uses AngelScript's line callback mechanism for cooperative interruption.
     *
     * Thread safety: configuration methods are not thread-safe (call from main
     * thread during setup). The line callback is invoked from the same thread
     * as script execution.
     */
    class ScriptSandbox
    {
      public:
        ScriptSandbox();

        /// Unregisters any console commands this sandbox registered (see
        /// RegisterConsoleCommands) so their [this]-capturing callbacks cannot
        /// outlive the sandbox in SimpleConsole.
        ~ScriptSandbox();

        // ====================================================================
        // Configuration
        // ====================================================================

        /// Set the security level (adjusts defaults accordingly)
        void SetSecurityLevel(ScriptSecurityLevel level);

        /// Get the current security level
        [[nodiscard]] ScriptSecurityLevel GetSecurityLevel() const { return m_securityLevel; }

        /// Set maximum instructions per script call (0 = unlimited)
        void SetInstructionLimit(uint32_t maxInstructions);

        /// Set maximum execution time per script call in seconds (0 = unlimited)
        void SetExecutionTimeout(float seconds);

        // ====================================================================
        // API Access Control
        // ====================================================================

        /// Add a function name to the allowed list (strict mode whitelist)
        void AddAllowedFunction(const std::string& name);

        /// Add a function name to the blocked list (standard mode blacklist)
        void AddBlockedFunction(const std::string& name);

        /// Remove a function from the allowed list
        void RemoveAllowedFunction(const std::string& name);

        /// Remove a function from the blocked list
        void RemoveBlockedFunction(const std::string& name);

        /// Check if a function name is allowed under current security settings
        [[nodiscard]] bool IsFunctionAllowed(const std::string& name) const;

        // ====================================================================
        // Execution Monitoring
        // ====================================================================

        /// Call before executing a script to reset per-call counters
        void BeginExecution(const std::string& scriptName);

        /// Call after script execution completes
        void EndExecution();

        /// Check if execution was terminated by the sandbox
        [[nodiscard]] bool WasTerminated() const { return m_wasTerminated; }

#ifdef SPARK_ANGELSCRIPT_SUPPORT
        /**
         * @brief AngelScript line callback — called periodically during execution
         *
         * Checks instruction count and elapsed time. Suspends the context
         * if either limit is exceeded.
         *
         * @param ctx The active AngelScript context
         * @param sandbox Pointer to this ScriptSandbox instance
         */
        static void LineCallback(asIScriptContext* ctx, void* sandbox);
#endif

        // ====================================================================
        // Violation Tracking
        // ====================================================================

        /// Get recent violations (thread-safe copy)
        [[nodiscard]] std::vector<SandboxViolation> GetViolations() const;

        /// Get total violation count
        [[nodiscard]] uint32_t GetTotalViolations() const;

        /// Clear violation history
        void ClearViolations();

        // ====================================================================
        // Console & Diagnostics
        // ====================================================================

        /// Register console commands (sandbox.status, sandbox.level, etc.)
        void RegisterConsoleCommands();

        /// Unregister console commands previously added by RegisterConsoleCommands.
        /// Called from the destructor; safe to call if nothing was registered.
        void UnregisterConsoleCommands();

        /// Get a human-readable status string
        [[nodiscard]] std::string GetStatusString() const;

      private:
        static constexpr size_t MAX_VIOLATION_HISTORY = 64;
        /// Approximate per-line instruction budget charged on each line-callback
        /// invocation. AngelScript's line callback fires per executed script line
        /// (not per instruction), so m_instructionCount is really (lines fired) x
        /// this weight — an approximate budget, not a true instruction count. The
        /// execution timeout is the precise backstop for heavy per-line work.
        static constexpr uint32_t LINE_CALLBACK_WEIGHT = 1000;

        ScriptSecurityLevel m_securityLevel = ScriptSecurityLevel::Standard;

        // Resource limits
        uint32_t m_maxInstructions = 1'000'000;
        float m_maxExecutionTimeSec = 0.1f;

        // Per-execution state
        uint32_t m_instructionCount = 0;
        std::chrono::steady_clock::time_point m_executionStart;
        std::string m_currentScript;
        bool m_wasTerminated = false;

        // Re-entrancy depth: only the outermost BeginExecution resets the
        // per-call counters, so a nested script Execute() shares (and cannot
        // reset) the outer frame's instruction/time budget.
        uint32_t m_executionDepth = 0;

        // True once RegisterConsoleCommands has added its commands, so the
        // destructor knows to remove them.
        bool m_consoleCommandsRegistered = false;

        // API access control
        std::unordered_set<std::string> m_allowedFunctions; ///< Whitelist (strict mode)
        std::unordered_set<std::string> m_blockedFunctions; ///< Blacklist (standard mode)

        // Violation history
        mutable std::mutex m_violationMutex;
        std::vector<SandboxViolation> m_violations;
        uint32_t m_totalViolations = 0;

        /// Record a violation and optionally log it
        void RecordViolation(SandboxViolationType type, const std::string& details);

        /// Apply defaults for the given security level
        void ApplySecurityDefaults(ScriptSecurityLevel level);
    };

} // namespace Spark
