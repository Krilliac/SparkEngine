/**
 * @file TFServerSim.cpp
 * @brief Authoritative fixed-tick simulation (movement validate, hits, capture). (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Net/TFServerSim.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFServerSim::TFServerSim() = default;
TFServerSim::~TFServerSim() { if (m_initialized) Shutdown(); }

bool TFServerSim::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFServerSim initialized (stub)");
    return true;
}

void TFServerSim::Update(float deltaTime) { (void)deltaTime; }
void TFServerSim::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFServerSim::Shutdown()
{
    m_initialized = false;
}

void TFServerSim::RenderDebugUI() {}

} // namespace Terrafront
