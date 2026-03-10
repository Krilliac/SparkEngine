/**
 * @file EngineContext.h
 * @brief Concrete implementation of IEngineContext for the engine runtime
 *
 * EngineContext wraps the engine's subsystem pointers (graphics, input, timer,
 * animation, AI, networking, scene, scripting, save, coroutine, etc.) behind
 * the IEngineContext interface. The engine creates one instance after
 * initializing subsystems and passes it to all loaded modules.
 */

#pragma once

#include "Spark/IEngineContext.h"

#include <typeindex>
#include <unordered_map>

class GraphicsEngine;
class InputManager;
class Timer;
class AudioEngine;
class PhysicsSystem;
class SceneManager;
class AngelScriptEngine;

/**
 * @brief Concrete IEngineContext - central service locator for all engine subsystems
 *
 * Owns no subsystems; holds non-owning pointers set at construction or via setters.
 * Subsystem lifetime is managed by SparkEngine's main function.
 *
 * For subsystems not covered by named getters, use the generic GetSystem<T>() /
 * RegisterSystem<T>() template methods.
 */
class EngineContext : public Spark::IEngineContext
{
  public:
    EngineContext() = default;
    EngineContext(GraphicsEngine* graphics, InputManager* input, Timer* timer, Spark::EventBus* eventBus = nullptr);
    ~EngineContext() override = default;

    // --- Core subsystems (original 6) ---

    GraphicsEngine* GetGraphics() override { return m_graphics; }
    const GraphicsEngine* GetGraphics() const override { return m_graphics; }
    InputManager* GetInput() override { return m_input; }
    const InputManager* GetInput() const override { return m_input; }
    Timer* GetTimer() override { return m_timer; }
    const Timer* GetTimer() const override { return m_timer; }
    Spark::EventBus* GetEventBus() override { return m_eventBus; }
    const Spark::EventBus* GetEventBus() const override { return m_eventBus; }
    AudioEngine* GetAudio() override { return m_audio; }
    const AudioEngine* GetAudio() const override { return m_audio; }
    PhysicsSystem* GetPhysics() override { return m_physics; }
    const PhysicsSystem* GetPhysics() const override { return m_physics; }

    // --- Extended subsystems (GAP-CI01) ---

    Spark::Animation::AnimationSystem* GetAnimation() override { return m_animation; }
    const Spark::Animation::AnimationSystem* GetAnimation() const override { return m_animation; }
    Spark::AI::AISystem* GetAI() override { return m_ai; }
    const Spark::AI::AISystem* GetAI() const override { return m_ai; }
    Spark::NetworkManager* GetNetwork() override { return m_network; }
    const Spark::NetworkManager* GetNetwork() const override { return m_network; }
    SceneManager* GetSceneManager() override { return m_sceneManager; }
    const SceneManager* GetSceneManager() const override { return m_sceneManager; }
    AngelScriptEngine* GetScriptEngine() override { return m_scriptEngine; }
    const AngelScriptEngine* GetScriptEngine() const override { return m_scriptEngine; }
    Spark::SaveSystem* GetSaveSystem() override { return m_saveSystem; }
    const Spark::SaveSystem* GetSaveSystem() const override { return m_saveSystem; }
    Spark::CoroutineScheduler* GetCoroutineScheduler() override { return m_coroutineScheduler; }
    const Spark::CoroutineScheduler* GetCoroutineScheduler() const override { return m_coroutineScheduler; }

    bool IsHeadless() const override;

    // --- Setters (core) ---

    void SetGraphics(GraphicsEngine* g) { m_graphics = g; }
    void SetInput(InputManager* i) { m_input = i; }
    void SetTimer(Timer* t) { m_timer = t; }
    void SetEventBus(Spark::EventBus* e) { m_eventBus = e; }
    void SetAudio(AudioEngine* a) { m_audio = a; }
    void SetPhysics(PhysicsSystem* p) { m_physics = p; }

    // --- Setters (extended) ---

    void SetAnimation(Spark::Animation::AnimationSystem* a) { m_animation = a; }
    void SetAI(Spark::AI::AISystem* a) { m_ai = a; }
    void SetNetwork(Spark::NetworkManager* n) { m_network = n; }
    void SetSceneManager(SceneManager* s) { m_sceneManager = s; }
    void SetScriptEngine(AngelScriptEngine* s) { m_scriptEngine = s; }
    void SetSaveSystem(Spark::SaveSystem* s) { m_saveSystem = s; }
    void SetCoroutineScheduler(Spark::CoroutineScheduler* c) { m_coroutineScheduler = c; }

    // --- Generic system registry for extensibility ---

    /**
     * @brief Register an arbitrary subsystem by type
     *
     * Allows registration of systems not covered by the named getters.
     * The pointer is non-owning; caller manages lifetime.
     */
    template <typename T> void RegisterSystem(T* system)
    {
        m_systems[std::type_index(typeid(T))] = static_cast<void*>(system);
    }

    /**
     * @brief Retrieve a previously registered subsystem by type
     * @return Pointer to the system, or nullptr if not registered
     */
    template <typename T> T* GetSystem() const
    {
        auto it = m_systems.find(std::type_index(typeid(T)));
        if (it != m_systems.end())
        {
            return static_cast<T*>(it->second);
        }
        return nullptr;
    }

    uint32_t GetEngineVersion() const override;
    uint32_t GetSDKVersion() const override;

  private:
    // Core subsystems
    GraphicsEngine* m_graphics = nullptr;
    InputManager* m_input = nullptr;
    Timer* m_timer = nullptr;
    Spark::EventBus* m_eventBus = nullptr;
    AudioEngine* m_audio = nullptr;
    PhysicsSystem* m_physics = nullptr;

    // Extended subsystems
    Spark::Animation::AnimationSystem* m_animation = nullptr;
    Spark::AI::AISystem* m_ai = nullptr;
    Spark::NetworkManager* m_network = nullptr;
    SceneManager* m_sceneManager = nullptr;
    AngelScriptEngine* m_scriptEngine = nullptr;
    Spark::SaveSystem* m_saveSystem = nullptr;
    Spark::CoroutineScheduler* m_coroutineScheduler = nullptr;

    // Generic system registry
    std::unordered_map<std::type_index, void*> m_systems;
};
