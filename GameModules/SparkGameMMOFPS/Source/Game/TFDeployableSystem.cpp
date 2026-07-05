/**
 * @file TFDeployableSystem.cpp
 * @brief Fabricator turret/ammo pack, Medtech beacon. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Game/TFDeployableSystem.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFDeployableSystem::TFDeployableSystem() = default;
TFDeployableSystem::~TFDeployableSystem() { if (m_initialized) Shutdown(); }

bool TFDeployableSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDeployableSystem initialized (stub)");
    return true;
}

void TFDeployableSystem::Update(float deltaTime) { (void)deltaTime; }
void TFDeployableSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFDeployableSystem::Shutdown()
{
    m_initialized = false;
}

void TFDeployableSystem::RenderDebugUI() {}

} // namespace Terrafront
