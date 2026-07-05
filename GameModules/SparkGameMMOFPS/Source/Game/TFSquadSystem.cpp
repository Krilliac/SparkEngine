/**
 * @file TFSquadSystem.cpp
 * @brief Squads of 6: invite/accept/leave, squad spawn, waypoint. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Game/TFSquadSystem.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFSquadSystem::TFSquadSystem() = default;
TFSquadSystem::~TFSquadSystem() { if (m_initialized) Shutdown(); }

bool TFSquadSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSquadSystem initialized (stub)");
    return true;
}

void TFSquadSystem::Update(float deltaTime) { (void)deltaTime; }
void TFSquadSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFSquadSystem::Shutdown()
{
    m_initialized = false;
}

void TFSquadSystem::RenderDebugUI() {}

} // namespace Terrafront
