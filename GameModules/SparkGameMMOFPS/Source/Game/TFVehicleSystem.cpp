/**
 * @file TFVehicleSystem.cpp
 * @brief Drifter/Aegis/Ravager: seats, driving, vehicle weapons. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Game/TFVehicleSystem.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFVehicleSystem::TFVehicleSystem() = default;
TFVehicleSystem::~TFVehicleSystem() { if (m_initialized) Shutdown(); }

bool TFVehicleSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFVehicleSystem initialized (stub)");
    return true;
}

void TFVehicleSystem::Update(float deltaTime) { (void)deltaTime; }
void TFVehicleSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFVehicleSystem::Shutdown()
{
    m_initialized = false;
}

void TFVehicleSystem::RenderDebugUI() {}

} // namespace Terrafront
