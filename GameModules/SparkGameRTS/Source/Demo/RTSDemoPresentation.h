/**
 * @file RTSDemoPresentation.h
 * @brief Playable RTS showcase setup, controls, fog updates, and battlefield UI
 *
 * With a world available it also stages the skirmish in 3D with the Blender-authored RTS kit
 * (Assets/Models/RTS/Kit): structures, rally flags, resource nodes and selection markers.
 */

#pragma once

#include "Enums/RTSEnums.h"

#include <cstdint>
#include <map>
#include <utility>

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
    class RTSSkirmishSimulation;
    class RTSUnitSystem;

    /** @brief Turns the RTS systems into an immediately playable live example. */
    class RTSDemoPresentation
    {
      public:
        bool Initialize(Spark::IEngineContext* context, RTSUnitSystem* units, RTSBuildingSystem* buildings,
                        RTSResourceSystem* resources, RTSCommandSystem* commands, RTSFogOfWarSystem* fog,
                        RTSMatchSystem* match, RTSSkirmishSimulation* simulation);
        void Shutdown();
        bool Reset();
        void UpdateInput();
        void RenderUI();
        /** @brief Place, move or remove the kit meshes so the world mirrors the simulation (call after it advances). */
        void SyncKitProps();

        void SelectUnitType(RTSUnitType type);
        void SelectArmy();
        bool MoveSelection(float x, float y, bool queued = false);
        bool HoldSelection();
        bool StopSelection();
        bool TrainMarine();

      private:
        bool IsPressed(int key, bool& heldState) const;
        void DrawBattlefield();
        void RemoveKitProps();

        Spark::IEngineContext* m_context{nullptr};
        RTSUnitSystem* m_units{nullptr};
        RTSBuildingSystem* m_buildings{nullptr};
        RTSResourceSystem* m_resources{nullptr};
        RTSCommandSystem* m_commands{nullptr};
        RTSFogOfWarSystem* m_fog{nullptr};
        RTSMatchSystem* m_match{nullptr};
        RTSSkirmishSimulation* m_simulation{nullptr};
        /// (prop kind, simulation id) -> MeshRenderer entity, owned by this presentation
        std::map<std::pair<uint8_t, uint32_t>, uint32_t> m_kitProps;
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
