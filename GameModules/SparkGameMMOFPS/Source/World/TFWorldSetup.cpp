/**
 * @file TFWorldSetup.cpp
 * @brief Scene/terrain load, WorldServer/AreaServer boot, origin-rebase driving. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "World/TFWorldSetup.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFWorldSetup::TFWorldSetup() = default;
TFWorldSetup::~TFWorldSetup() { if (m_initialized) Shutdown(); }

bool TFWorldSetup::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFWorldSetup initialized (stub)");
    return true;
}

void TFWorldSetup::Update(float deltaTime) { (void)deltaTime; }
void TFWorldSetup::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFWorldSetup::Shutdown()
{
    m_initialized = false;
}

void TFWorldSetup::RenderDebugUI() {}

} // namespace Terrafront
