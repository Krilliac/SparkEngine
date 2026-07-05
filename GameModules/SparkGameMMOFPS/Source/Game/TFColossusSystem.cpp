/**
 * @file TFColossusSystem.cpp
 * @brief Colossus exosuit purchase + suit stats. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Game/TFColossusSystem.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFColossusSystem::TFColossusSystem() = default;
TFColossusSystem::~TFColossusSystem() { if (m_initialized) Shutdown(); }

bool TFColossusSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFColossusSystem initialized (stub)");
    return true;
}

void TFColossusSystem::Update(float deltaTime) { (void)deltaTime; }
void TFColossusSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFColossusSystem::Shutdown()
{
    m_initialized = false;
}

void TFColossusSystem::RenderDebugUI() {}

} // namespace Terrafront
