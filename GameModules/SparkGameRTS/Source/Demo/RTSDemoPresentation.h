/**
 * @file RTSDemoPresentation.h
 * @brief Playable RTS showcase setup, controls, fog updates, and battlefield UI
 */

#pragma once

#include "Enums/RTSEnums.h"

#include <cstdint>

namespace Spark
{
    class IEngineContext;
}

namespace RTS
{
    class RTSBuildingSystem;
    class RTSCommandSystem;
    class RTSFogOfWarSystem;
    class RTSMatchSystem;
    class RTSResourceSystem;
    class RTSUnitSystem;

    /** @brief Turns the RTS systems into an immediately playable live example. */
    class RTSDemoPresentation
    {
      public:
        bool Initialize(Spark::IEngineContext* context, RTSUnitSystem* units, RTSBuildingSystem* buildings,
                        RTSResourceSystem* resources, RTSCommandSystem* commands, RTSFogOfWarSystem* fog,
                        RTSMatchSystem* match);
        void Shutdown();
        bool Reset();
        void UpdateInput();
        void RefreshVision();
        void RenderUI();

        void SelectUnitType(RTSUnitType type);
        void SelectArmy();
        bool MoveSelection(float x, float y, bool queued = false);
        bool HoldSelection();
        bool StopSelection();
        bool TrainMarine();

      private:
        bool IsPressed(int key, bool& heldState) const;
        void DrawBattlefield();

        Spark::IEngineContext* m_context{nullptr};
        RTSUnitSystem* m_units{nullptr};
        RTSBuildingSystem* m_buildings{nullptr};
        RTSResourceSystem* m_resources{nullptr};
        RTSCommandSystem* m_commands{nullptr};
        RTSFogOfWarSystem* m_fog{nullptr};
        RTSMatchSystem* m_match{nullptr};
        uint32_t m_waypointIndex{0};
        bool m_workerHeld{false};
        bool m_marineHeld{false};
        bool m_tankHeld{false};
        bool m_moveHeld{false};
        bool m_holdHeld{false};
        bool m_stopHeld{false};
        bool m_restartHeld{false};
    };

} // namespace RTS
