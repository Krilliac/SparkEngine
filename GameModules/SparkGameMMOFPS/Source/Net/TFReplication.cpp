/**
 * @file TFReplication.cpp
 * @brief EntityReplicator wiring; TF component <-> replication field mapping. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Net/TFReplication.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFReplication::TFReplication() = default;
TFReplication::~TFReplication() { if (m_initialized) Shutdown(); }

bool TFReplication::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFReplication initialized (stub)");
    return true;
}

void TFReplication::Update(float deltaTime) { (void)deltaTime; }
void TFReplication::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFReplication::Shutdown()
{
    m_initialized = false;
}

void TFReplication::RenderDebugUI() {}

} // namespace Terrafront
