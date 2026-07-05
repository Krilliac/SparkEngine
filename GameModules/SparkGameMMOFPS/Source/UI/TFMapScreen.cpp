/**
 * @file TFMapScreen.cpp
 * @brief Continent hex map: ownership, lattice, deploy/redeploy. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "UI/TFMapScreen.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFMapScreen::TFMapScreen() = default;
TFMapScreen::~TFMapScreen() { if (m_initialized) Shutdown(); }

bool TFMapScreen::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFMapScreen initialized (stub)");
    return true;
}

void TFMapScreen::Update(float deltaTime) { (void)deltaTime; }
void TFMapScreen::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFMapScreen::Shutdown()
{
    m_initialized = false;
}

void TFMapScreen::RenderDebugUI() {}
void TFMapScreen::RenderUI() {}

} // namespace Terrafront
