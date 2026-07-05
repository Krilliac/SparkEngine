/**
 * @file TFHUD.cpp
 * @brief ImGui HUD: health/shield/ammo, crosshair, hitmarkers, killfeed, minimap. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "UI/TFHUD.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFHUD::TFHUD() = default;
TFHUD::~TFHUD() { if (m_initialized) Shutdown(); }

bool TFHUD::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFHUD initialized (stub)");
    return true;
}

void TFHUD::Update(float deltaTime) { (void)deltaTime; }
void TFHUD::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFHUD::Shutdown()
{
    m_initialized = false;
}

void TFHUD::RenderDebugUI() {}
void TFHUD::RenderUI() {}

} // namespace Terrafront
