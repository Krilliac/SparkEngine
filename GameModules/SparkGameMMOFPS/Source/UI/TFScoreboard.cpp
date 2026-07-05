/**
 * @file TFScoreboard.cpp
 * @brief Per-faction K/D/score/regions tab screen. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "UI/TFScoreboard.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFScoreboard::TFScoreboard() = default;
TFScoreboard::~TFScoreboard() { if (m_initialized) Shutdown(); }

bool TFScoreboard::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFScoreboard initialized (stub)");
    return true;
}

void TFScoreboard::Update(float deltaTime) { (void)deltaTime; }
void TFScoreboard::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFScoreboard::Shutdown()
{
    m_initialized = false;
}

void TFScoreboard::RenderDebugUI() {}
void TFScoreboard::RenderUI() {}

} // namespace Terrafront
