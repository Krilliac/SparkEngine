/**
 * @file TFDataTables.cpp
 * @brief JSON data-table loaders (weapons/vehicles/classes/regions/factions). (W0 stub — implementation lands per DESIGN.md waves.)
 */
#include "Data/TFDataTables.h"
#include "Utils/LogMacros.h"

namespace Terrafront {

TFDataTables::TFDataTables() = default;
TFDataTables::~TFDataTables() { if (m_initialized) Shutdown(); }

bool TFDataTables::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDataTables initialized (stub)");
    return true;
}

void TFDataTables::Update(float deltaTime) { (void)deltaTime; }
void TFDataTables::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFDataTables::Shutdown()
{
    m_initialized = false;
}

void TFDataTables::RenderDebugUI() {}

} // namespace Terrafront
