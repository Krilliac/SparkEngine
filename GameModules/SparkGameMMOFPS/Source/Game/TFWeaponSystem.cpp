/**
 * @file TFWeaponSystem.cpp
 * @brief Data-driven weapons: hitscan + projectile, ADS/recoil/spread. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Game/TFWeaponSystem.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFWeaponSystem::TFWeaponSystem() = default;
TFWeaponSystem::~TFWeaponSystem() { if (m_initialized) Shutdown(); }

bool TFWeaponSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFWeaponSystem initialized (stub)");
    return true;
}

void TFWeaponSystem::Update(float deltaTime) { (void)deltaTime; }
void TFWeaponSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFWeaponSystem::Shutdown()
{
    m_initialized = false;
}

void TFWeaponSystem::RenderDebugUI() {}

} // namespace Terrafront
