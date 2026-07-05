/**
 * @file TFSpawnScreen.cpp
 * @brief Death -> spawn point chooser (skyanchor/base/Aegis/squad). (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "UI/TFSpawnScreen.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFSpawnScreen::TFSpawnScreen() = default;
TFSpawnScreen::~TFSpawnScreen() { if (m_initialized) Shutdown(); }

bool TFSpawnScreen::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSpawnScreen initialized (stub)");
    return true;
}

void TFSpawnScreen::Update(float deltaTime) { (void)deltaTime; }
void TFSpawnScreen::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFSpawnScreen::Shutdown()
{
    m_initialized = false;
}

void TFSpawnScreen::RenderDebugUI() {}
void TFSpawnScreen::RenderUI() {}

} // namespace Terrafront
