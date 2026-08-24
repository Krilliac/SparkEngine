/**
 * @file DebugHookManager.h
 * @brief Project-wide debugging hook system for engine instrumentation
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a centralized hook dispatch system for debugging and profiling across
 * all engine subsystems. Unlike ScriptHookManager (which handles gameplay hooks
 * like combat and quests), DebugHookManager instruments the engine itself:
 * system initialization, frame boundaries, per-system updates, resource loading,
 * state transitions, and error conditions.
 *
 * ## Design
 * - Hook points are inserted at key engine lifecycle moments (init, update, shutdown)
 * - Handlers are priority-ordered (lower = runs first) and can inspect context
 * - RAII handles for automatic unregistration
 * - Conditional hooks: only fire when a predicate returns true (e.g. frame > N)
 * - Hooks compile to no-ops in shipping builds via SPARK_DEBUG_HOOKS_ENABLED
 * - Thread-safe: all registration/dispatch is mutex-protected
 *
 * ## Usage
 * @code
 *   auto& hooks = Spark::DebugHookManager::GetInstance();
 *
 *   // Log every frame that takes longer than 16ms
 *   auto handle = hooks.Register(DebugHookPoint::FrameEnd, "SlowFrameDetector",
 *       [](const DebugHookContext& ctx) {
 *           if (ctx.deltaTime > 0.016f)
 *               SPARK_LOG_WARN(LogCategory::Core, "Slow frame: %.1fms", ctx.deltaTime * 1000.f);
 *       });
 *
 *   // Monitor physics system updates
 *   auto h2 = hooks.Register(DebugHookPoint::SystemPostUpdate, "PhysicsWatchdog",
 *       [](const DebugHookContext& ctx) {
 *           if (ctx.systemName == "Physics")
 *               SPARK_LOG_DEBUG(LogCategory::Physics, "Physics update: %.2fms", ctx.durationMs);
 *       });
 *
 *   // handle auto-unregisters on destruction (RAII)
 * @endcode
 *
 * @see ScriptHookManager.h for gameplay hook dispatch (combat, quests, entity lifecycle)
 * @see EventBus.h for the type-safe publish/subscribe event system
 * @see Profiler.h for frame timing and performance analysis
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Debug hooks are active in Debug/Development builds, compiled out in Shipping
#ifndef SPARK_SHIPPING
#define SPARK_DEBUG_HOOKS_ENABLED 1
#else
#define SPARK_DEBUG_HOOKS_ENABLED 0
#endif

namespace Spark
{

    // =========================================================================
    // Hook Points
    // =========================================================================

    /**
     * @brief Engine-level instrumentation points where debug hooks can fire.
     *
     * These correspond to well-defined moments in the engine lifecycle.
     * Each hook point carries a DebugHookContext with relevant data.
     */
    enum class DebugHookPoint : uint32_t
    {
        // Engine lifecycle
        EnginePreInit,      ///< Before any subsystem initialization
        EnginePostInit,     ///< After all subsystems are initialized
        EnginePreShutdown,  ///< Before shutdown sequence begins
        EnginePostShutdown, ///< After all subsystems are shut down

        // Frame boundaries
        FrameBegin, ///< Start of a new frame (before any updates)
        FrameEnd,   ///< End of frame (after render, before swap)

        // System lifecycle
        SystemPreInit,      ///< Before a specific subsystem initializes
        SystemPostInit,     ///< After a specific subsystem initializes
        SystemPreUpdate,    ///< Before a specific subsystem updates
        SystemPostUpdate,   ///< After a specific subsystem updates (durationMs filled)
        SystemPreShutdown,  ///< Before a specific subsystem shuts down
        SystemPostShutdown, ///< After a specific subsystem shuts down

        // Resource events
        ResourceLoadBegin,    ///< A resource started loading (resourceName filled)
        ResourceLoadComplete, ///< A resource finished loading (durationMs filled)
        ResourceUnload,       ///< A resource is being unloaded

        // State transitions
        ScenePreLoad,    ///< Before a scene/level loads
        ScenePostLoad,   ///< After a scene/level loads
        ScenePreUnload,  ///< Before a scene/level unloads
        ScenePostUnload, ///< After a scene/level unloads

        // Error/warning conditions
        ErrorRaised,   ///< An engine error occurred (message filled)
        WarningRaised, ///< An engine warning occurred (message filled)

        // Fault isolation
        SubsystemFaulted,   ///< A subsystem was disabled after repeated faults (systemName filled)
        SubsystemRecovered, ///< A faulted subsystem was re-enabled (systemName filled)

        // Device recovery
        DeviceLostFallback, ///< GPU device lost, falling back to headless mode
        DeviceRecovered,    ///< GPU device successfully recovered after loss

        // Memory
        AllocationSpike, ///< Memory allocation exceeded a threshold

        Count
    };

    /**
     * @brief Convert DebugHookPoint to a human-readable string.
     */
    constexpr std::string_view DebugHookPointToString(DebugHookPoint point) noexcept
    {
        switch (point)
        {
        case DebugHookPoint::EnginePreInit:
            return "EnginePreInit";
        case DebugHookPoint::EnginePostInit:
            return "EnginePostInit";
        case DebugHookPoint::EnginePreShutdown:
            return "EnginePreShutdown";
        case DebugHookPoint::EnginePostShutdown:
            return "EnginePostShutdown";
        case DebugHookPoint::FrameBegin:
            return "FrameBegin";
        case DebugHookPoint::FrameEnd:
            return "FrameEnd";
        case DebugHookPoint::SystemPreInit:
            return "SystemPreInit";
        case DebugHookPoint::SystemPostInit:
            return "SystemPostInit";
        case DebugHookPoint::SystemPreUpdate:
            return "SystemPreUpdate";
        case DebugHookPoint::SystemPostUpdate:
            return "SystemPostUpdate";
        case DebugHookPoint::SystemPreShutdown:
            return "SystemPreShutdown";
        case DebugHookPoint::SystemPostShutdown:
            return "SystemPostShutdown";
        case DebugHookPoint::ResourceLoadBegin:
            return "ResourceLoadBegin";
        case DebugHookPoint::ResourceLoadComplete:
            return "ResourceLoadComplete";
        case DebugHookPoint::ResourceUnload:
            return "ResourceUnload";
        case DebugHookPoint::ScenePreLoad:
            return "ScenePreLoad";
        case DebugHookPoint::ScenePostLoad:
            return "ScenePostLoad";
        case DebugHookPoint::ScenePreUnload:
            return "ScenePreUnload";
        case DebugHookPoint::ScenePostUnload:
            return "ScenePostUnload";
        case DebugHookPoint::ErrorRaised:
            return "ErrorRaised";
        case DebugHookPoint::WarningRaised:
            return "WarningRaised";
        case DebugHookPoint::DeviceLostFallback:
            return "DeviceLostFallback";
        case DebugHookPoint::DeviceRecovered:
            return "DeviceRecovered";
        case DebugHookPoint::AllocationSpike:
            return "AllocationSpike";
        case DebugHookPoint::SubsystemFaulted:
            return "SubsystemFaulted";
        case DebugHookPoint::SubsystemRecovered:
            return "SubsystemRecovered";
        default:
            return "Unknown";
        }
    }

    // =========================================================================
    // Hook Context
    // =========================================================================

    /**
     * @brief Data passed to debug hook handlers when a hook point fires.
     *
     * The engine fills in the relevant fields before dispatch. Not all fields
     * are meaningful for every hook point — check the hook point documentation.
     */
    struct DebugHookContext
    {
        DebugHookPoint point{};        ///< Which hook point triggered this
        uint64_t frameNumber = 0;      ///< Current engine frame number
        float deltaTime = 0.0f;        ///< Frame delta time (seconds)
        double durationMs = 0.0;       ///< Duration of the operation (for Post* hooks)
        std::string_view systemName;   ///< Name of the subsystem (for System* hooks)
        std::string_view resourceName; ///< Resource path/name (for Resource* hooks)
        std::string_view sceneName;    ///< Scene name (for Scene* hooks)
        std::string_view message;      ///< Error/warning message (for Error/Warning hooks)
        size_t allocationBytes = 0;    ///< Allocation size (for AllocationSpike)
    };

    // =========================================================================
    // Debug Hook Handle (RAII)
    // =========================================================================

    class DebugHookManager; // forward

    /**
     * @brief RAII handle that automatically unregisters a debug hook on destruction.
     *
     * Move-only. Calling Unregister() explicitly or letting the handle destruct
     * both cleanly remove the hook from the manager.
     */
    class DebugHookHandle
    {
      public:
        DebugHookHandle() = default;

        DebugHookHandle(const DebugHookHandle&) = delete;
        DebugHookHandle& operator=(const DebugHookHandle&) = delete;

        DebugHookHandle(DebugHookHandle&& other) noexcept : m_manager(other.m_manager), m_id(other.m_id)
        {
            other.m_manager = nullptr;
            other.m_id = 0;
        }

        DebugHookHandle& operator=(DebugHookHandle&& other) noexcept
        {
            if (this != &other)
            {
                Unregister();
                m_manager = other.m_manager;
                m_id = other.m_id;
                other.m_manager = nullptr;
                other.m_id = 0;
            }
            return *this;
        }

        ~DebugHookHandle() { Unregister(); }

        /** @brief Manually unregister. Safe to call multiple times. */
        void Unregister();

        /** @brief Whether this handle is currently registered. */
        [[nodiscard]] bool IsActive() const noexcept { return m_id != 0 && m_manager != nullptr; }

      private:
        friend class DebugHookManager;

        DebugHookHandle(DebugHookManager* manager, uint32_t id) : m_manager(manager), m_id(id) {}

        DebugHookManager* m_manager = nullptr;
        uint32_t m_id = 0;
    };

    // =========================================================================
    // Debug Hook Manager
    // =========================================================================

    /** @brief Callback signature for debug hook handlers. */
    using DebugHookHandler = std::function<void(const DebugHookContext&)>;

    /**
     * @brief Central manager for project-wide debugging hooks.
     *
     * Singleton. The engine calls Dispatch() at instrumentation points throughout
     * the codebase. External code registers handlers via Register() and receives
     * RAII handles for automatic cleanup.
     *
     * All methods are thread-safe. Dispatch copies the handler list under lock,
     * then invokes handlers without holding the lock (same pattern as ScriptHookManager).
     */
    class DebugHookManager
    {
      public:
        /** @brief Get the process-lifetime singleton instance. */
        static DebugHookManager& GetInstance();

        /**
         * @brief Register a debug hook handler.
         * @param point    The hook point to listen for.
         * @param name     Descriptive name (for logging/debugging).
         * @param handler  Callback invoked when the hook fires.
         * @param priority Execution order; lower values run first. Default 0.
         * @return RAII handle that unregisters on destruction.
         */
        [[nodiscard]] DebugHookHandle Register(DebugHookPoint point, const std::string& name, DebugHookHandler handler,
                                               int priority = 0);

        /**
         * @brief Unregister a hook by its ID. Prefer using the RAII handle.
         * @param id The registration ID returned internally.
         */
        void Unregister(uint32_t id);

        /**
         * @brief Remove all hooks registered with a specific name.
         * @param name The name passed during Register().
         */
        void UnregisterAllByName(const std::string& name);

        /**
         * @brief Dispatch a hook point, invoking all registered handlers.
         *
         * Handlers run in priority order. The context is passed by const reference.
         *
         * @param context Pre-filled context with hook point data.
         */
        void Dispatch(const DebugHookContext& context);

        /**
         * @brief Convenience: dispatch a simple hook point with just frame info.
         * @param point       The hook point to fire.
         * @param frameNumber Current frame number.
         * @param deltaTime   Frame delta time in seconds.
         */
        void Dispatch(DebugHookPoint point, uint64_t frameNumber = 0, float deltaTime = 0.0f);

        /**
         * @brief Convenience: dispatch a system-related hook.
         * @param point       SystemPreInit, SystemPostInit, SystemPreUpdate, etc.
         * @param systemName  Name of the subsystem.
         * @param durationMs  Duration in milliseconds (for Post* hooks).
         */
        void DispatchSystem(DebugHookPoint point, std::string_view systemName, double durationMs = 0.0);

        /**
         * @brief Convenience: dispatch a resource-related hook.
         * @param point        ResourceLoadBegin, ResourceLoadComplete, ResourceUnload.
         * @param resourceName Path or name of the resource.
         * @param durationMs   Load duration in milliseconds (for ResourceLoadComplete).
         */
        void DispatchResource(DebugHookPoint point, std::string_view resourceName, double durationMs = 0.0);

        /**
         * @brief Convenience: dispatch a scene transition hook.
         * @param point     ScenePreLoad, ScenePostLoad, etc.
         * @param sceneName Name of the scene.
         */
        void DispatchScene(DebugHookPoint point, std::string_view sceneName);

        /**
         * @brief Convenience: dispatch an error or warning hook.
         * @param point   ErrorRaised or WarningRaised.
         * @param message The error/warning message.
         */
        void DispatchDebugMessage(DebugHookPoint point, std::string_view message);

        /** @brief Whether any hooks are registered for a specific point. */
        [[nodiscard]] bool HasHandlers(DebugHookPoint point) const;

        /** @brief Number of handlers registered for a specific point. */
        [[nodiscard]] int GetHandlerCount(DebugHookPoint point) const;

        /** @brief Total number of handlers across all hook points. */
        [[nodiscard]] int GetTotalHandlerCount() const;

        /** @brief Master enable/disable toggle. When disabled, Dispatch is a no-op. */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        /** @brief Whether the hook manager is currently enabled. */
        [[nodiscard]] bool IsEnabled() const { return m_enabled; }

        /** @brief Get the current frame number (set by the engine each frame). */
        [[nodiscard]] uint64_t GetFrameNumber() const { return m_frameNumber; }

        /** @brief Set the current frame number (called by the engine each frame). */
        void SetFrameNumber(uint64_t frame) { m_frameNumber = frame; }

        /** @brief Get the current delta time. */
        [[nodiscard]] float GetDeltaTime() const { return m_deltaTime; }

        /** @brief Set the current delta time (called by the engine each frame). */
        void SetDeltaTime(float dt) { m_deltaTime = dt; }

        /** @brief Remove all registered hooks. */
        void Clear();

      private:
        DebugHookManager() = default;

        struct HookEntry
        {
            uint32_t id = 0;
            DebugHookPoint point{};
            std::string name;
            DebugHookHandler handler;
            int priority = 0;
        };

        mutable std::mutex m_mutex;
        std::unordered_map<DebugHookPoint, std::vector<HookEntry>> m_hooks;
        std::atomic<uint32_t> m_nextId{1};
        std::atomic<bool> m_enabled{true};
        std::atomic<uint64_t> m_frameNumber{0};
        std::atomic<float> m_deltaTime{0.0f};
    };

    // =========================================================================
    // Convenience Macros
    // =========================================================================

    // In non-shipping builds, these macros dispatch debug hooks at instrumentation points.
    // In shipping builds, they compile to nothing.

#if SPARK_DEBUG_HOOKS_ENABLED

#define SPARK_DEBUG_HOOK(point, frameNum, dt)                                                                          \
    Spark::DebugHookManager::GetInstance().Dispatch(Spark::DebugHookPoint::point, frameNum, dt)

#define SPARK_DEBUG_HOOK_SYSTEM(point, sysName, durationMs)                                                            \
    Spark::DebugHookManager::GetInstance().DispatchSystem(Spark::DebugHookPoint::point, sysName, durationMs)

#define SPARK_DEBUG_HOOK_RESOURCE(point, resName, durationMs)                                                          \
    Spark::DebugHookManager::GetInstance().DispatchResource(Spark::DebugHookPoint::point, resName, durationMs)

#define SPARK_DEBUG_HOOK_SCENE(point, sceneName)                                                                       \
    Spark::DebugHookManager::GetInstance().DispatchScene(Spark::DebugHookPoint::point, sceneName)

#define SPARK_DEBUG_HOOK_MESSAGE(point, msg)                                                                           \
    Spark::DebugHookManager::GetInstance().DispatchDebugMessage(Spark::DebugHookPoint::point, msg)

#else

#define SPARK_DEBUG_HOOK(point, frameNum, dt) ((void)0)
#define SPARK_DEBUG_HOOK_SYSTEM(point, sysName, durationMs) ((void)0)
#define SPARK_DEBUG_HOOK_RESOURCE(point, resName, durationMs) ((void)0)
#define SPARK_DEBUG_HOOK_SCENE(point, sceneName) ((void)0)
#define SPARK_DEBUG_HOOK_MESSAGE(point, msg) ((void)0)

#endif

} // namespace Spark
