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

#include "Core/Contracts.h"

#include <algorithm>
#include <any>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
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
class AssetPipeline;
class World;

// ============================================================================
// Compile-time type ID (works with incomplete/forward-declared types)
// ============================================================================

/**
 * @brief Unique type identifier that does not require complete types
 *
 * Uses the address of a per-type static variable as a unique key.
 * Unlike std::type_index / typeid, this works with forward-declared types.
 */
#ifndef SPARK_TYPEID_DEFINED
#define SPARK_TYPEID_DEFINED
using TypeId = const void*;

template <typename T> TypeId GetTypeId()
{
    // IMPORTANT: this marker must be non-const. A `static const char id = 0` is an
    // identical read-only COMDAT for every T, which MSVC's /OPT:ICF (enabled in
    // Release, disabled in Debug) folds into a single address — collapsing every
    // type id to the same value and making the service locator return the wrong
    // subsystem in Release builds. Writable data is not ICF-folded, so each T
    // keeps a distinct address. Do not add `const` back.
    static char id;
    return &id;
}
#endif

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
     * @brief Install a non-owning EngineContext pointer for the current module image
     *
     * SparkEngineLib is a static library linked into every game-module DLL, so the
     * owning @c g_engineContext global is per-image and is null inside modules. The
     * host injects its live EngineContext across the DLL boundary via this setter so
     * that @c Get() inside a module returns the engine's real context instead of a
     * dead per-image instance. Passing a raw (non-owning) pointer here — never the
     * owning unique_ptr — is deliberate: module teardown must not free the host's
     * EngineContext. @c Get() prefers the injected pointer when one has been set.
     *
     * @param ctx Non-owning pointer to the host EngineContext (may be nullptr to clear)
     */
    static void SetInjected(EngineContext* ctx);

    /**
     * @brief Access the owning unique_ptr (read-only)
     *
     * Returns a const reference to prevent external code from accidentally
     * moving or resetting the singleton. Use Get() for non-owning access,
     * SetOwned() to initialize, and ResetOwned() to tear down.
     */
    static const std::unique_ptr<EngineContext>& GetOwned();

    /**
     * @brief Set the global EngineContext (initialization only)
     * @param ctx The new EngineContext to install as the global singleton
     */
    static void SetOwned(std::unique_ptr<EngineContext> ctx);

    /**
     * @brief Reset the global EngineContext (shutdown only)
     */
    static void ResetOwned();

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
    Spark::INetworkService* GetNetworkService() override { return GetSystem<Spark::INetworkService>(); }
    const Spark::INetworkService* GetNetworkService() const override { return GetSystem<Spark::INetworkService>(); }
    Spark::ITelemetryService* GetTelemetryService() override { return GetSystem<Spark::ITelemetryService>(); }
    const Spark::ITelemetryService* GetTelemetryService() const override
    {
        return GetSystem<Spark::ITelemetryService>();
    }
    Spark::IGameplayTagService* GetGameplayTagService() override { return GetSystem<Spark::IGameplayTagService>(); }
    const Spark::IGameplayTagService* GetGameplayTagService() const override
    {
        return GetSystem<Spark::IGameplayTagService>();
    }
    ::World* GetWorld() override { return GetSystem<::World>(); }
    const ::World* GetWorld() const override { return GetSystem<::World>(); }
    SceneManager* GetSceneManager() override { return GetSystem<SceneManager>(); }
    const SceneManager* GetSceneManager() const override { return GetSystem<SceneManager>(); }
    AngelScriptEngine* GetScriptEngine() override { return GetSystem<AngelScriptEngine>(); }
    const AngelScriptEngine* GetScriptEngine() const override { return GetSystem<AngelScriptEngine>(); }
    ::AssetPipeline* GetAssetPipeline() override { return GetSystem<::AssetPipeline>(); }
    const ::AssetPipeline* GetAssetPipeline() const override { return GetSystem<::AssetPipeline>(); }
    Spark::SaveSystem* GetSaveSystem() override { return GetSystem<Spark::SaveSystem>(); }
    const Spark::SaveSystem* GetSaveSystem() const override { return GetSystem<Spark::SaveSystem>(); }
    Spark::CoroutineScheduler* GetCoroutineScheduler() override { return GetSystem<Spark::CoroutineScheduler>(); }
    const Spark::CoroutineScheduler* GetCoroutineScheduler() const override
    {
        return GetSystem<Spark::CoroutineScheduler>();
    }
    Spark::LocalFileCache* GetFileCache() override { return GetSystem<Spark::LocalFileCache>(); }
    const Spark::LocalFileCache* GetFileCache() const override { return GetSystem<Spark::LocalFileCache>(); }
    Spark::AssetRegistry* GetAssetRegistry() override { return GetSystem<Spark::AssetRegistry>(); }
    const Spark::AssetRegistry* GetAssetRegistry() const override { return GetSystem<Spark::AssetRegistry>(); }
    Spark::WeatherSystem* GetWeather() override { return GetSystem<Spark::WeatherSystem>(); }
    const Spark::WeatherSystem* GetWeather() const override { return GetSystem<Spark::WeatherSystem>(); }
    Spark::TimeOfDaySystem* GetTimeOfDay() override { return GetSystem<Spark::TimeOfDaySystem>(); }
    const Spark::TimeOfDaySystem* GetTimeOfDay() const override { return GetSystem<Spark::TimeOfDaySystem>(); }
    Spark::UI::UISystem* GetUI() override { return GetSystem<Spark::UI::UISystem>(); }
    const Spark::UI::UISystem* GetUI() const override { return GetSystem<Spark::UI::UISystem>(); }
    Spark::DialogueSystem* GetDialogue() override { return GetSystem<Spark::DialogueSystem>(); }
    const Spark::DialogueSystem* GetDialogue() const override { return GetSystem<Spark::DialogueSystem>(); }
    Spark::ModSystem* GetModSystem() override { return GetSystem<Spark::ModSystem>(); }
    const Spark::ModSystem* GetModSystem() const override { return GetSystem<Spark::ModSystem>(); }

    Spark::ReplaySystem* GetReplay() override { return GetSystem<Spark::ReplaySystem>(); }
    const Spark::ReplaySystem* GetReplay() const override { return GetSystem<Spark::ReplaySystem>(); }
    Spark::LocalizationSystem* GetLocalization() override { return GetSystem<Spark::LocalizationSystem>(); }
    const Spark::LocalizationSystem* GetLocalization() const override { return GetSystem<Spark::LocalizationSystem>(); }
    Spark::TweenSystem* GetTween() override { return GetSystem<Spark::TweenSystem>(); }
    const Spark::TweenSystem* GetTween() const override { return GetSystem<Spark::TweenSystem>(); }
    Spark::Gameplay::AbilitySystem* GetAbilities() override { return GetSystem<Spark::Gameplay::AbilitySystem>(); }
    const Spark::Gameplay::AbilitySystem* GetAbilities() const override
    {
        return GetSystem<Spark::Gameplay::AbilitySystem>();
    }
    Spark::DestructionSystem* GetDestruction() override { return GetSystem<Spark::DestructionSystem>(); }
    const Spark::DestructionSystem* GetDestruction() const override { return GetSystem<Spark::DestructionSystem>(); }
    Spark::Cinematic::SequencerManager* GetCinematic() override
    {
        return GetSystem<Spark::Cinematic::SequencerManager>();
    }
    const Spark::Cinematic::SequencerManager* GetCinematic() const override
    {
        return GetSystem<Spark::Cinematic::SequencerManager>();
    }
    Spark::VR::VRSystem* GetVR() override { return GetSystem<Spark::VR::VRSystem>(); }
    const Spark::VR::VRSystem* GetVR() const override { return GetSystem<Spark::VR::VRSystem>(); }

    SparkEngineCamera* GetCamera() override { return GetSystem<SparkEngineCamera>(); }
    const SparkEngineCamera* GetCamera() const override { return GetSystem<SparkEngineCamera>(); }
    Spark::Gameplay::WeaponSystem* GetWeapons() override { return GetSystem<Spark::Gameplay::WeaponSystem>(); }
    const Spark::Gameplay::WeaponSystem* GetWeapons() const override
    {
        return GetSystem<Spark::Gameplay::WeaponSystem>();
    }
    Spark::Gameplay::ConditionSystem* GetConditions() override { return GetSystem<Spark::Gameplay::ConditionSystem>(); }
    const Spark::Gameplay::ConditionSystem* GetConditions() const override
    {
        return GetSystem<Spark::Gameplay::ConditionSystem>();
    }
    Spark::Gameplay::InstanceManager* GetInstances() override { return GetSystem<Spark::Gameplay::InstanceManager>(); }
    const Spark::Gameplay::InstanceManager* GetInstances() const override
    {
        return GetSystem<Spark::Gameplay::InstanceManager>();
    }

    Spark::Streaming::SeamlessAreaManager* GetAreaStreaming() override
    {
        return GetSystem<Spark::Streaming::SeamlessAreaManager>();
    }
    const Spark::Streaming::SeamlessAreaManager* GetAreaStreaming() const override
    {
        return GetSystem<Spark::Streaming::SeamlessAreaManager>();
    }
    Spark::Audio::MusicManager* GetMusic() override { return GetSystem<Spark::Audio::MusicManager>(); }
    const Spark::Audio::MusicManager* GetMusic() const override { return GetSystem<Spark::Audio::MusicManager>(); }
    Spark::VirtualFileSystem* GetVFS() override { return GetSystem<Spark::VirtualFileSystem>(); }
    const Spark::VirtualFileSystem* GetVFS() const override { return GetSystem<Spark::VirtualFileSystem>(); }

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
    void SetNetworkService(Spark::INetworkService* s) { RegisterSystem<Spark::INetworkService>(s); }
    void SetTelemetryService(Spark::ITelemetryService* s) { RegisterSystem<Spark::ITelemetryService>(s); }
    void SetGameplayTagService(Spark::IGameplayTagService* s) { RegisterSystem<Spark::IGameplayTagService>(s); }
    void SetWorld(World* w) { RegisterSystem<World>(w); }
    void SetSceneManager(SceneManager* s) { RegisterSystem<SceneManager>(s); }
    void SetScriptEngine(AngelScriptEngine* s) { RegisterSystem<AngelScriptEngine>(s); }
    void SetAssetPipeline(::AssetPipeline* a) { RegisterSystem<::AssetPipeline>(a); }
    void SetSaveSystem(Spark::SaveSystem* s) { RegisterSystem<Spark::SaveSystem>(s); }
    void SetCoroutineScheduler(Spark::CoroutineScheduler* c) { RegisterSystem<Spark::CoroutineScheduler>(c); }
    void SetFileCache(Spark::LocalFileCache* f) { RegisterSystem<Spark::LocalFileCache>(f); }
    void SetAssetRegistry(Spark::AssetRegistry* a) { RegisterSystem<Spark::AssetRegistry>(a); }
    void SetWeather(Spark::WeatherSystem* w) { RegisterSystem<Spark::WeatherSystem>(w); }
    void SetTimeOfDay(Spark::TimeOfDaySystem* t) { RegisterSystem<Spark::TimeOfDaySystem>(t); }
    void SetUI(Spark::UI::UISystem* u) { RegisterSystem<Spark::UI::UISystem>(u); }
    void SetDialogue(Spark::DialogueSystem* d) { RegisterSystem<Spark::DialogueSystem>(d); }
    void SetModSystem(Spark::ModSystem* m) { RegisterSystem<Spark::ModSystem>(m); }
    void SetReplay(Spark::ReplaySystem* r) { RegisterSystem<Spark::ReplaySystem>(r); }
    void SetLocalization(Spark::LocalizationSystem* l) { RegisterSystem<Spark::LocalizationSystem>(l); }
    void SetTween(Spark::TweenSystem* t) { RegisterSystem<Spark::TweenSystem>(t); }
    void SetAbilities(Spark::Gameplay::AbilitySystem* a) { RegisterSystem<Spark::Gameplay::AbilitySystem>(a); }
    void SetDestruction(Spark::DestructionSystem* d) { RegisterSystem<Spark::DestructionSystem>(d); }
    void SetCinematic(Spark::Cinematic::SequencerManager* c) { RegisterSystem<Spark::Cinematic::SequencerManager>(c); }
    void SetVR(Spark::VR::VRSystem* v) { RegisterSystem<Spark::VR::VRSystem>(v); }
    void SetCamera(SparkEngineCamera* c) { RegisterSystem<SparkEngineCamera>(c); }
    void SetWeapons(Spark::Gameplay::WeaponSystem* w) { RegisterSystem<Spark::Gameplay::WeaponSystem>(w); }
    void SetConditions(Spark::Gameplay::ConditionSystem* c) { RegisterSystem<Spark::Gameplay::ConditionSystem>(c); }
    void SetInstances(Spark::Gameplay::InstanceManager* i) { RegisterSystem<Spark::Gameplay::InstanceManager>(i); }
    void SetAreaStreaming(Spark::Streaming::SeamlessAreaManager* a)
    {
        RegisterSystem<Spark::Streaming::SeamlessAreaManager>(a);
    }
    void SetMusic(Spark::Audio::MusicManager* m) { RegisterSystem<Spark::Audio::MusicManager>(m); }
    void SetVFS(Spark::VirtualFileSystem* v) { RegisterSystem<Spark::VirtualFileSystem>(v); }

    // =========================================================================
    // Generic system registry (R1.1 — single source of truth)
    // =========================================================================

    /**
     * @brief Register an arbitrary subsystem by type
     *
     * Stores a non-owning pointer in the generic registry. Passing nullptr
     * unregisters the type, which lets named setters clear subsystem pointers
     * during teardown. The caller manages lifetime. This is the single source
     * of truth for all subsystem pointers. Works with incomplete
     * (forward-declared) types.
     */
    template <typename T> void RegisterSystem(T* system)
    {
        std::unique_lock<std::shared_mutex> lock(m_systemsMutex);
        const TypeId typeId = GetTypeId<T>();
        if (system == nullptr)
        {
            m_systems.erase(typeId);
            return;
        }
        m_systems[typeId] = static_cast<void*>(system);
    }

    /**
     * @brief Retrieve a previously registered subsystem by type
     * @return Pointer to the system, or nullptr if not registered
     * Works with incomplete (forward-declared) types.
     */
    template <typename T> T* GetSystem() const
    {
        std::shared_lock<std::shared_mutex> lock(m_systemsMutex);
        auto it = m_systems.find(GetTypeId<T>());
        if (it != m_systems.end())
        {
            return static_cast<T*>(it->second);
        }
        return nullptr;
    }

    /**
     * @brief Retrieve a subsystem with a logged warning if not registered
     *
     * Use this variant when the caller expects the subsystem to be present.
     * Returns nullptr with a diagnostic log message if missing, making it
     * easier to track down initialization-order bugs.
     *
     * @param callerName  Name of the calling function (for diagnostics)
     * @return Pointer to the system, or nullptr if not registered (with warning logged)
     */
    template <typename T> T* GetSystemChecked(const char* callerName = nullptr) const
    {
        T* system = GetSystem<T>();
        if (!system)
        {
            // Log through stderr since we can't depend on Logger being available
            // (Logger itself might be the missing subsystem). Warn only once per
            // missing type T — callers that poll an optional subsystem every frame
            // would otherwise flood stderr and mask real errors. The static is per
            // template instantiation, so each distinct T logs exactly once.
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true, std::memory_order_relaxed))
            {
                std::fprintf(stderr, "[EngineContext] WARNING: Subsystem not registered (requested by %s)\n",
                             callerName ? callerName : "unknown");
            }
        }
        return system;
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
        SubsystemEntry entry{GetTypeId<T>(), {}, std::move(depList), std::move(initFn), std::move(shutdownFn), false};

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

    // Generic system registry (void* with TypeId key) — single source of truth.
    // Guarded by m_systemsMutex: startup is single-threaded, but module hot-reload
    // can RegisterSystem at runtime while game/render/network threads GetSystem, and
    // a concurrent insert that rehashes during a reader's find() is UB.
    mutable std::unordered_map<TypeId, void*, TypeIdHash> m_systems;
    mutable std::shared_mutex m_systemsMutex;

    // Dependency-aware subsystem entries (R1.2)
    std::vector<SubsystemEntry> m_subsystemEntries;

    // Cached initialization order (populated by InitializeAll)
    std::vector<TypeId> m_initOrder;
};
