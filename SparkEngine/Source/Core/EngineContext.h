/**
 * @file EngineContext.h
 * @brief Concrete implementation of IEngineContext for the engine runtime
 *
 * EngineContext wraps the engine's subsystem pointers behind the IEngineContext
 * interface using a unified generic registry as the single source of truth.
 * Named getters (GetGraphics, GetInput, etc.) are preserved for backward
 * compatibility but delegate to the generic registry internally.
 *
 * R1.1: All named getters/setters now delegate to the generic registry.
 * R1.2: Dependency-aware subsystem initialization via RegisterSubsystem<T>(),
 *        InitializeAll(), and ShutdownAll().
 */

#pragma once

#include "Spark/IEngineContext.h"

#include <algorithm>
#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class GraphicsEngine;
class InputManager;
class Timer;
class AudioEngine;
class PhysicsSystem;
class SceneManager;
class AngelScriptEngine;

// ============================================================================
// Compile-time type ID (works with incomplete/forward-declared types)
// ============================================================================

/**
 * @brief Unique type identifier that does not require complete types
 *
 * Uses the address of a per-type static variable as a unique key.
 * Unlike std::type_index / typeid, this works with forward-declared types.
 */
using TypeId = const void*;

template <typename T> TypeId GetTypeId()
{
    static const char id = 0;
    return &id;
}

// ============================================================================
// Dependency declaration helpers (R1.2)
// ============================================================================

/**
 * @brief Tag type used to declare subsystem dependencies at registration time
 *
 * Usage: ctx->RegisterSubsystem<MySystem>(&sys, DependsOn<Timer, EventBus>{});
 * An empty DependsOn<>{} means no dependencies.
 */
template <typename... Deps> struct DependsOn
{
};

/**
 * @brief Metadata about a registered subsystem for dependency-aware init
 */
struct SubsystemEntry
{
    TypeId type = nullptr;
    std::string name;
    std::vector<TypeId> dependencies;
    std::function<bool()> initFn;     ///< Called during InitializeAll()
    std::function<void()> shutdownFn; ///< Called during ShutdownAll()
    bool initialized = false;
};

// ============================================================================
// Hash for TypeId (const void*) to use in unordered containers
// ============================================================================

struct TypeIdHash
{
    size_t operator()(TypeId id) const noexcept { return std::hash<const void*>{}(id); }
};

/**
 * @brief Concrete IEngineContext - central service locator for all engine subsystems
 *
 * Uses a single generic registry (TypeId -> void*) as the source of truth.
 * Named getters delegate to this registry, providing backward compatibility while
 * eliminating duplicated state.
 *
 * For subsystems not covered by named getters, use GetSystem<T>() /
 * RegisterSystem<T>() template methods.
 */
class EngineContext : public Spark::IEngineContext
{
  public:
    EngineContext() = default;
    EngineContext(GraphicsEngine* graphics, InputManager* input, Timer* timer, Spark::EventBus* eventBus = nullptr);
    ~EngineContext() override = default;

    /**
     * @brief Global accessor for the singleton EngineContext instance
     * @return Pointer to the global EngineContext, or nullptr if not yet created
     */
    static EngineContext* Get();

    /**
     * @brief Access the owning unique_ptr (for initialization/shutdown only)
     */
    static std::unique_ptr<EngineContext>& GetOwned();

    // =========================================================================
    // Named getters — delegate to generic registry (R1.1)
    // =========================================================================

    GraphicsEngine* GetGraphics() override { return GetSystem<GraphicsEngine>(); }
    const GraphicsEngine* GetGraphics() const override { return GetSystem<GraphicsEngine>(); }
    InputManager* GetInput() override { return GetSystem<InputManager>(); }
    const InputManager* GetInput() const override { return GetSystem<InputManager>(); }
    Timer* GetTimer() override { return GetSystem<Timer>(); }
    const Timer* GetTimer() const override { return GetSystem<Timer>(); }
    Spark::EventBus* GetEventBus() override { return GetSystem<Spark::EventBus>(); }
    const Spark::EventBus* GetEventBus() const override { return GetSystem<Spark::EventBus>(); }
    AudioEngine* GetAudio() override { return GetSystem<AudioEngine>(); }
    const AudioEngine* GetAudio() const override { return GetSystem<AudioEngine>(); }
    PhysicsSystem* GetPhysics() override { return GetSystem<PhysicsSystem>(); }
    const PhysicsSystem* GetPhysics() const override { return GetSystem<PhysicsSystem>(); }

    Spark::Animation::AnimationSystem* GetAnimation() override
    {
        return GetSystem<Spark::Animation::AnimationSystem>();
    }
    const Spark::Animation::AnimationSystem* GetAnimation() const override
    {
        return GetSystem<Spark::Animation::AnimationSystem>();
    }
    Spark::AI::AISystem* GetAI() override { return GetSystem<Spark::AI::AISystem>(); }
    const Spark::AI::AISystem* GetAI() const override { return GetSystem<Spark::AI::AISystem>(); }
    Spark::NetworkManager* GetNetwork() override { return GetSystem<Spark::NetworkManager>(); }
    const Spark::NetworkManager* GetNetwork() const override { return GetSystem<Spark::NetworkManager>(); }
    SceneManager* GetSceneManager() override { return GetSystem<SceneManager>(); }
    const SceneManager* GetSceneManager() const override { return GetSystem<SceneManager>(); }
    AngelScriptEngine* GetScriptEngine() override { return GetSystem<AngelScriptEngine>(); }
    const AngelScriptEngine* GetScriptEngine() const override { return GetSystem<AngelScriptEngine>(); }
    Spark::SaveSystem* GetSaveSystem() override { return GetSystem<Spark::SaveSystem>(); }
    const Spark::SaveSystem* GetSaveSystem() const override { return GetSystem<Spark::SaveSystem>(); }
    Spark::CoroutineScheduler* GetCoroutineScheduler() override { return GetSystem<Spark::CoroutineScheduler>(); }
    const Spark::CoroutineScheduler* GetCoroutineScheduler() const override
    {
        return GetSystem<Spark::CoroutineScheduler>();
    }

    bool IsHeadless() const override;

    // =========================================================================
    // Named setters — delegate to generic registry (R1.1)
    // =========================================================================

    void SetGraphics(GraphicsEngine* g) { RegisterSystem<GraphicsEngine>(g); }
    void SetInput(InputManager* i) { RegisterSystem<InputManager>(i); }
    void SetTimer(Timer* t) { RegisterSystem<Timer>(t); }
    void SetEventBus(Spark::EventBus* e) { RegisterSystem<Spark::EventBus>(e); }
    void SetAudio(AudioEngine* a) { RegisterSystem<AudioEngine>(a); }
    void SetPhysics(PhysicsSystem* p) { RegisterSystem<PhysicsSystem>(p); }
    void SetAnimation(Spark::Animation::AnimationSystem* a) { RegisterSystem<Spark::Animation::AnimationSystem>(a); }
    void SetAI(Spark::AI::AISystem* a) { RegisterSystem<Spark::AI::AISystem>(a); }
    void SetNetwork(Spark::NetworkManager* n) { RegisterSystem<Spark::NetworkManager>(n); }
    void SetSceneManager(SceneManager* s) { RegisterSystem<SceneManager>(s); }
    void SetScriptEngine(AngelScriptEngine* s) { RegisterSystem<AngelScriptEngine>(s); }
    void SetSaveSystem(Spark::SaveSystem* s) { RegisterSystem<Spark::SaveSystem>(s); }
    void SetCoroutineScheduler(Spark::CoroutineScheduler* c) { RegisterSystem<Spark::CoroutineScheduler>(c); }

    // =========================================================================
    // Generic system registry (R1.1 — single source of truth)
    // =========================================================================

    /**
     * @brief Register an arbitrary subsystem by type
     *
     * Stores a non-owning pointer in the generic registry. The caller manages
     * lifetime. This is the single source of truth for all subsystem pointers.
     * Works with incomplete (forward-declared) types.
     */
    template <typename T> void RegisterSystem(T* system) { m_systems[GetTypeId<T>()] = static_cast<void*>(system); }

    /**
     * @brief Retrieve a previously registered subsystem by type
     * @return Pointer to the system, or nullptr if not registered
     * Works with incomplete (forward-declared) types.
     */
    template <typename T> T* GetSystem() const
    {
        auto it = m_systems.find(GetTypeId<T>());
        if (it != m_systems.end())
        {
            return static_cast<T*>(it->second);
        }
        return nullptr;
    }

    // =========================================================================
    // Dependency-aware subsystem registration and lifecycle (R1.2)
    // =========================================================================

    /**
     * @brief Register a subsystem with dependency metadata for ordered init/shutdown
     *
     * @tparam T        The subsystem type
     * @tparam Deps     Types this subsystem depends on (declared via DependsOn<...>)
     * @param system    Non-owning pointer to the subsystem
     * @param deps      DependsOn<...> tag (types are extracted at compile time)
     * @param initFn    Optional initialization callback (called during InitializeAll)
     * @param shutdownFn Optional shutdown callback (called during ShutdownAll)
     */
    template <typename T, typename... Deps>
    void RegisterSubsystem(T* system, DependsOn<Deps...> /*deps*/, std::function<bool()> initFn = nullptr,
                           std::function<void()> shutdownFn = nullptr)
    {
        // Store in the generic registry
        RegisterSystem<T>(system);

        // Build dependency list
        std::vector<TypeId> depList;
        (depList.push_back(GetTypeId<Deps>()), ...);

        // Store entry for topological sorting
        SubsystemEntry entry{GetTypeId<T>(),    typeid(T).name(),      std::move(depList),
                             std::move(initFn), std::move(shutdownFn), false};

        // Replace existing entry for same type, or append
        auto it = std::find_if(m_subsystemEntries.begin(), m_subsystemEntries.end(),
                               [&](const SubsystemEntry& e) { return e.type == GetTypeId<T>(); });
        if (it != m_subsystemEntries.end())
        {
            *it = std::move(entry);
        }
        else
        {
            m_subsystemEntries.push_back(std::move(entry));
        }
    }

    /**
     * @brief Initialize all registered subsystems in topological (dependency) order
     *
     * Performs a topological sort of subsystem entries based on their declared
     * dependencies. Subsystems with no init callback are silently skipped.
     *
     * @return true if all subsystems initialized successfully, false on failure or cycle
     */
    bool InitializeAll() override;

    /**
     * @brief Shut down all initialized subsystems in reverse dependency order
     *
     * Iterates the initialization order in reverse, calling each subsystem's
     * shutdown callback. Subsystems that were not initialized are skipped.
     */
    void ShutdownAll() override;

    /**
     * @brief Get the computed initialization order (for debugging/testing)
     * @return Vector of TypeId in topological order, empty if not yet computed
     */
    const std::vector<TypeId>& GetInitOrder() const { return m_initOrder; }

    /**
     * @brief Get the number of registered subsystem entries
     */
    size_t GetSubsystemCount() const { return m_subsystemEntries.size(); }

    uint32_t GetEngineVersion() const override;
    uint32_t GetSDKVersion() const override;

  private:
    /**
     * @brief Perform topological sort of subsystem entries
     * @param[out] sorted  Resulting order
     * @return true if sort succeeded (no cycles), false if a dependency cycle was detected
     */
    bool TopologicalSort(std::vector<SubsystemEntry*>& sorted);

    // Generic system registry (void* with TypeId key) — single source of truth
    mutable std::unordered_map<TypeId, void*, TypeIdHash> m_systems;

    // Dependency-aware subsystem entries (R1.2)
    std::vector<SubsystemEntry> m_subsystemEntries;

    // Cached initialization order (populated by InitializeAll)
    std::vector<TypeId> m_initOrder;
};
