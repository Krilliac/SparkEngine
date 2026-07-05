/**
 * @file TFDamageSystem.cpp
 * @brief Health/shields, TTK model, friendly fire, kill credit. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Game/TFDamageSystem.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFDamageSystem::TFDamageSystem() = default;
TFDamageSystem::~TFDamageSystem() { if (m_initialized) Shutdown(); }

bool TFDamageSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDamageSystem initialized (stub)");
    return true;
}

void TFDamageSystem::Update(float deltaTime) { (void)deltaTime; }
void TFDamageSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFDamageSystem::Shutdown()
{
    m_initialized = false;
}

void TFDamageSystem::RenderDebugUI() {}

} // namespace Terrafront
