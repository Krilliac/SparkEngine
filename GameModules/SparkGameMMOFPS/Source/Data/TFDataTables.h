/**
 * @file TFDataTables.h
 * @brief JSON data-table loaders (weapons/vehicles/classes/regions/factions).
 *
 * OWNERSHIP: this header + TFDataTables.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W0: load + validate Assets/MMOFPS/Data/*.json; hot reload via tf_reload_data; typed accessors.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFDataTables {
  public:
    TFDataTables();
    ~TFDataTables();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

  private:
    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
};

} // namespace Terrafront
