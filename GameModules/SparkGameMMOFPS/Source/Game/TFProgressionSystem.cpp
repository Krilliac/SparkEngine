/**
 * @file TFProgressionSystem.cpp
 * @brief XP events, ranks 1-30, flux income, persistence. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Game/TFProgressionSystem.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFProgressionSystem::TFProgressionSystem() = default;
TFProgressionSystem::~TFProgressionSystem() { if (m_initialized) Shutdown(); }

bool TFProgressionSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFProgressionSystem initialized (stub)");
    return true;
}

void TFProgressionSystem::Update(float deltaTime) { (void)deltaTime; }
void TFProgressionSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFProgressionSystem::Shutdown()
{
    m_initialized = false;
}

void TFProgressionSystem::RenderDebugUI() {}

} // namespace Terrafront
