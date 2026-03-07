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

class EngineContext : public Spark::IEngineContext
{
public:
    EngineContext(GraphicsEngine* graphics, InputManager* input, Timer* timer,
                 Spark::EventBus* eventBus = nullptr);
    ~EngineContext() override = default;

    GraphicsEngine*  GetGraphics()  override { return m_graphics; }
    InputManager*    GetInput()     override { return m_input; }
    Timer*           GetTimer()     override { return m_timer; }
    Spark::EventBus* GetEventBus()  override { return m_eventBus; }

    uint32_t GetEngineVersion() const override;
    uint32_t GetSDKVersion()    const override;

private:
    GraphicsEngine*  m_graphics  = nullptr;
    InputManager*    m_input     = nullptr;
    Timer*           m_timer     = nullptr;
    Spark::EventBus* m_eventBus  = nullptr;
};
