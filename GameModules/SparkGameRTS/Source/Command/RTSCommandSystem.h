/**
 * @file RTSCommandSystem.h
 * @brief Unit commands: selection, move, attack, patrol, hold, build, gather
 * @author Spark Engine Team
 * @date 2026
 *
 * Handles issuing commands to units, managing selection groups, and
 * processing command queues each frame.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RTSEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RTS
{

    class RTSUnitSystem;

    /// @brief A command issued to a unit
    struct UnitCommand
    {
        RTSCommandType type = RTSCommandType::Stop;
        float targetX = 0.0f;
        float targetY = 0.0f;
        uint32_t targetEntity = 0; ///< Target unit or building ID (for attack/gather)
    };

    /**
     * @brief Manages unit selection, command issuing, and command processing
     */
    class RTSCommandSystem
    {
      public:
        RTSCommandSystem() = default;
        ~RTSCommandSystem() = default;

        bool Initialize(Spark::IEngineContext* context, RTSUnitSystem* unitSystem);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // === Selection ===
        void Select(uint32_t unitId);
        void AddToSelection(uint32_t unitId);
        void DeselectAll();
        void SelectAllOfType(RTSUnitType type, RTSFaction faction);
        const std::vector<uint32_t>& GetSelection() const;
        size_t GetSelectionCount() const;

        // === Commands ===
        void IssueCommand(uint32_t unitId, const UnitCommand& command);
        void QueueCommand(uint32_t unitId, const UnitCommand& command);
        void ClearCommands(uint32_t unitId);
        void IssueCommandToSelection(const UnitCommand& command);

        // === Queries ===
        const UnitCommand* GetCurrentCommand(uint32_t unitId) const;
        size_t GetPendingCommandCount() const;
        std::string GetCommandStatusString() const;

      private:
        void ProcessCommands(float deltaTime);
        [[nodiscard]] bool IsCommandValid(const UnitCommand& command) const;
        void PruneSelection();

        Spark::IEngineContext* m_context{nullptr};
        RTSUnitSystem* m_unitSystem{nullptr};

        // Current selection
        std::vector<uint32_t> m_selectedUnits;

        // Per-unit command queues
        std::unordered_map<uint32_t, std::vector<UnitCommand>> m_commandQueues;
    };

} // namespace RTS
