/**
 * @file EngineBootstrap.h
 * @brief Dependency-ordered subsystem initialization and shutdown
 *
 * EngineBootstrap accepts a set of SubsystemDescriptor entries, topologically
 * sorts them by declared dependencies, and walks the sorted list to initialize
 * (and later shutdown in reverse order) each subsystem.
 *
 * This replaces ad-hoc init/shutdown ordering scattered across main() and
 * ensures every subsystem is started only after its prerequisites are live.
 *
 * @code
 *   EngineBootstrap bootstrap;
 *   bootstrap.Register({ "Logger", InitLogger, ShutdownLogger, {} });
 *   bootstrap.Register({ "Graphics", InitGfx, ShutdownGfx, {"Logger", "Platform"} });
 *   if (bootstrap.Initialize())
 *       RunEngine();
 *   bootstrap.Shutdown();
 * @endcode
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    // ============================================================================
    // SubsystemDescriptor
    // ============================================================================

    /**
     * @brief Describes a single engine subsystem for bootstrap registration
     *
     * Each descriptor carries the subsystem name, callable init/shutdown hooks,
     * and a list of dependency names that must be initialized first.
     */
    struct SubsystemDescriptor
    {
        /** Human-readable subsystem name (must be unique across all descriptors) */
        std::string Name;

        /**
         * @brief Initialization callback
         * @return true on success, false on failure (bootstrap will log and skip dependents)
         */
        std::function<bool()> InitFunction;

        /** Shutdown callback (called in reverse init order) */
        std::function<void()> ShutdownFunction;

        /** Names of subsystems that must be initialized before this one */
        std::vector<std::string> Dependencies;
    };

    // ============================================================================
    // SubsystemState
    // ============================================================================

    /** @brief Runtime state of a subsystem managed by EngineBootstrap */
    enum class SubsystemState : uint8_t
    {
        Registered,  ///< Descriptor added but not yet initialized
        Initialized, ///< Init callback succeeded
        Failed,      ///< Init callback returned false or a dependency failed
        ShutDown     ///< Shutdown callback has been called
    };

    // ============================================================================
    // EngineBootstrap
    // ============================================================================

    /**
     * @brief Manages dependency-ordered initialization and shutdown of engine subsystems
     *
     * Typical usage:
     *  1. Register() all subsystem descriptors (order does not matter).
     *  2. Call Initialize() which topologically sorts by dependencies and calls
     *     each init function in the resulting order.
     *  3. At engine exit call Shutdown() which walks the successfully-initialized
     *     subsystems in reverse order.
     *
     * Thread safety: EngineBootstrap is designed for single-threaded use during
     * engine startup and shutdown. Do not call from multiple threads concurrently.
     */
    class EngineBootstrap
    {
      public:
        EngineBootstrap() = default;
        ~EngineBootstrap();

        /** Prevent copies -- bootstrap owns the init/shutdown lifecycle */
        EngineBootstrap(const EngineBootstrap&) = delete;
        EngineBootstrap& operator=(const EngineBootstrap&) = delete;
        EngineBootstrap(EngineBootstrap&&) noexcept = default;
        EngineBootstrap& operator=(EngineBootstrap&&) noexcept = default;

        // -----------------------------------------------------------------
        // Registration
        // -----------------------------------------------------------------

        /**
         * @brief Register a subsystem descriptor
         * @param descriptor The subsystem to register
         * @return true if registration succeeded, false if a descriptor with the
         *         same name was already registered
         *
         * Must be called before Initialize(). Calling after Initialize() is an error.
         */
        bool Register(SubsystemDescriptor descriptor);

        /**
         * @brief Register multiple descriptors at once
         * @param descriptors Initializer list of descriptors
         * @return Number of descriptors successfully registered
         */
        size_t RegisterAll(std::vector<SubsystemDescriptor> descriptors);

        // -----------------------------------------------------------------
        // Lifecycle
        // -----------------------------------------------------------------

        /**
         * @brief Topologically sort registered subsystems and initialize them
         *
         * Subsystems whose dependencies are missing or failed will be marked
         * Failed and their init function will not be called.
         *
         * @return true if ALL subsystems initialized successfully, false if any failed
         */
        bool Initialize();

        /**
         * @brief Shut down all successfully-initialized subsystems in reverse order
         *
         * Safe to call multiple times; subsequent calls are no-ops.
         */
        void Shutdown();

        // -----------------------------------------------------------------
        // Queries
        // -----------------------------------------------------------------

        /** @brief Check whether a named subsystem was successfully initialized */
        [[nodiscard]] bool IsInitialized(std::string_view name) const;

        /** @brief Get the state of a named subsystem */
        [[nodiscard]] SubsystemState GetState(std::string_view name) const;

        /** @brief Return the initialization order (after Initialize() has been called) */
        [[nodiscard]] const std::vector<std::string>& GetInitOrder() const;

        /** @brief Return the number of registered subsystems */
        [[nodiscard]] size_t GetSubsystemCount() const;

      private:
        // -----------------------------------------------------------------
        // Internal types
        // -----------------------------------------------------------------

        struct SubsystemEntry
        {
            SubsystemDescriptor Descriptor;
            SubsystemState State = SubsystemState::Registered;
        };

        // -----------------------------------------------------------------
        // Internal helpers
        // -----------------------------------------------------------------

        /**
         * @brief Topological sort via Kahn's algorithm
         * @param[out] sortedNames Receives the sorted subsystem names
         * @return true if the sort succeeded (no cycles), false on cycle detection
         */
        bool TopologicalSort(std::vector<std::string>& sortedNames) const;

        /**
         * @brief Find a SubsystemEntry by name
         * @return Pointer to the entry, or nullptr if not found
         */
        SubsystemEntry* FindEntry(std::string_view name);
        const SubsystemEntry* FindEntry(std::string_view name) const;

        // -----------------------------------------------------------------
        // Data
        // -----------------------------------------------------------------

        /** All registered subsystem entries, keyed by insertion order */
        std::vector<SubsystemEntry> m_entries;

        /** Initialization order produced by TopologicalSort (valid after Initialize()) */
        std::vector<std::string> m_initOrder;

        /** Names of subsystems that were successfully initialized (shutdown walks this in reverse) */
        std::vector<std::string> m_initializedSubsystems;

        /** Guard against double-init or register-after-init */
        bool m_initialized = false;

        /** Guard against double-shutdown */
        bool m_shutDown = false;
    };

} // namespace Spark
