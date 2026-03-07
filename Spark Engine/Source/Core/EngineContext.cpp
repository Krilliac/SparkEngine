/**
 * @file EngineContext.cpp
 * @brief Concrete IEngineContext implementation
 */

#include "EngineContext.h"
#include "Spark/Version.h"

EngineContext::EngineContext(GraphicsEngine* graphics, InputManager* input, Timer* timer)
    : m_graphics(graphics)
    , m_input(input)
    , m_timer(timer)
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
