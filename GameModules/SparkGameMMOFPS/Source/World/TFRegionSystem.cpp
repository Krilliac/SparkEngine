/**
 * @file TFRegionSystem.cpp
 * @brief Hex regions, conduit lattice, capture points, Dominion. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "World/TFRegionSystem.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFRegionSystem::TFRegionSystem() = default;
TFRegionSystem::~TFRegionSystem() { if (m_initialized) Shutdown(); }

bool TFRegionSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFRegionSystem initialized (stub)");
    return true;
}

void TFRegionSystem::Update(float deltaTime) { (void)deltaTime; }
void TFRegionSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFRegionSystem::Shutdown()
{
    m_initialized = false;
}

void TFRegionSystem::RenderDebugUI() {}

} // namespace Terrafront
