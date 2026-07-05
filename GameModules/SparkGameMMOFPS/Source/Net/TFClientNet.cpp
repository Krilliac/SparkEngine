/**
 * @file TFClientNet.cpp
 * @brief Client connection, ClientPrediction wiring, interpolation buffers. (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Net/TFClientNet.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFClientNet::TFClientNet() = default;
TFClientNet::~TFClientNet() { if (m_initialized) Shutdown(); }

bool TFClientNet::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFClientNet initialized (stub)");
    return true;
}

void TFClientNet::Update(float deltaTime) { (void)deltaTime; }
void TFClientNet::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFClientNet::Shutdown()
{
    m_initialized = false;
}

void TFClientNet::RenderDebugUI() {}

} // namespace Terrafront
