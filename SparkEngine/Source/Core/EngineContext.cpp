/**
 * @file EngineContext.cpp
 * @brief Concrete IEngineContext implementation
 */

#include "EngineContext.h"
#include "SparkEngine.h"
#include "Spark/Version.h"
#include <memory>

// Global engine context - defined here (in SparkEngineLib) so that all
// consumers of the static library (both the executable and SparkGame DLL)
// can resolve this symbol at link time.
std::unique_ptr<EngineContext> g_engineContext;

EngineContext::EngineContext(GraphicsEngine* graphics, InputManager* input, Timer* timer, Spark::EventBus* eventBus)
    : m_graphics(graphics), m_input(input), m_timer(timer), m_eventBus(eventBus)
{
}

uint32_t EngineContext::GetEngineVersion() const
{
    return Spark::GetEngineVersion();
}

uint32_t EngineContext::GetSDKVersion() const
{
    return Spark::GetSDKVersion();
}

bool EngineContext::IsHeadless() const
{
#ifdef SPARK_HEADLESS_SUPPORT
    extern bool g_headlessMode;
    return g_headlessMode;
#else
    return false;
#endif
}
