/**
 * @file EngineContext.h
 * @brief Concrete implementation of IEngineContext for the engine runtime
 *
 * EngineContext wraps the engine's subsystem pointers (graphics, input, timer)
 * behind the IEngineContext interface. The engine creates one instance after
 * initializing subsystems and passes it to all loaded modules.
 */

#pragma once

#include "Spark/IEngineContext.h"

class GraphicsEngine;
class InputManager;
class Timer;
class AudioEngine;
class PhysicsSystem;

/**
 * @brief Concrete IEngineContext - central service locator for all engine subsystems
 *
 * Owns no subsystems; holds non-owning pointers set at construction or via setters.
 * Subsystem lifetime is managed by SparkEngine's main function.
 */
class EngineContext : public Spark::IEngineContext
{
public:
    EngineContext() = default;
    EngineContext(GraphicsEngine* graphics, InputManager* input, Timer* timer,
                 Spark::EventBus* eventBus = nullptr);
    ~EngineContext() override = default;

    GraphicsEngine*  GetGraphics()  override { return m_graphics; }
    InputManager*    GetInput()     override { return m_input; }
    Timer*           GetTimer()     override { return m_timer; }
    Spark::EventBus* GetEventBus()  override { return m_eventBus; }
    AudioEngine*     GetAudio()     override { return m_audio; }
    PhysicsSystem*   GetPhysics()   override { return m_physics; }

    void SetGraphics(GraphicsEngine* g) { m_graphics = g; }
    void SetInput(InputManager* i)      { m_input = i; }
    void SetTimer(Timer* t)             { m_timer = t; }
    void SetEventBus(Spark::EventBus* e){ m_eventBus = e; }
    void SetAudio(AudioEngine* a)       { m_audio = a; }
    void SetPhysics(PhysicsSystem* p)   { m_physics = p; }

    uint32_t GetEngineVersion() const override;
    uint32_t GetSDKVersion()    const override;

private:
    GraphicsEngine*  m_graphics  = nullptr;
    InputManager*    m_input     = nullptr;
    Timer*           m_timer     = nullptr;
    Spark::EventBus* m_eventBus  = nullptr;
    AudioEngine*     m_audio     = nullptr;
    PhysicsSystem*   m_physics   = nullptr;
};
