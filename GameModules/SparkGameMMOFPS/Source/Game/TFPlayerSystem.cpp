/**
 * @file TFPlayerSystem.cpp
 * @brief Spawn/death/respawn, class loadouts, movement tuning. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Game/TFPlayerSystem.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFPlayerSystem::TFPlayerSystem() = default;
TFPlayerSystem::~TFPlayerSystem() { if (m_initialized) Shutdown(); }

bool TFPlayerSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFPlayerSystem initialized (stub)");
    return true;
}

void TFPlayerSystem::Update(float deltaTime) { (void)deltaTime; }
void TFPlayerSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFPlayerSystem::Shutdown()
{
    m_initialized = false;
}

void TFPlayerSystem::RenderDebugUI() {}

} // namespace Terrafront
