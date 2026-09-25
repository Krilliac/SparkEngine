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
#include <map>
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
        uint32_t targetEntity = 0; ///< Target unit or building ID; an Attack with no target is an attack-move
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

        /** @brief Per-unit command queues in ascending unit-id order (the processing order). */
        const std::map<uint32_t, std::vector<UnitCommand>>& GetCommandQueues() const;

        /**
         * @brief Replace the selection and every command queue from a validated persistence snapshot.
         *
         * Ids of units that no longer exist are accepted: a unit killed mid-tick keeps its queue and selection
         * entry until the next Update prunes them, and a resumed match must reproduce that exactly.
         * @return false (leaving state untouched) on a zero id, a duplicate selection entry, an empty queue, or a
         *         command IssueCommand would reject.
         */
        bool RestoreRuntimeState(const std::map<uint32_t, std::vector<UnitCommand>>& queues,
                                 const std::vector<uint32_t>& selection);

        /** @brief The acceptance rule IssueCommand and QueueCommand apply to every order. */
        [[nodiscard]] static bool IsCommandValid(const UnitCommand& command);

        static constexpr size_t MAX_QUEUED_COMMANDS = 64; ///< Per-unit shift-queue limit; later orders are dropped

      private:
        void ProcessCommands(float deltaTime);
        void PruneSelection();

        Spark::IEngineContext* m_context{nullptr};
        RTSUnitSystem* m_unitSystem{nullptr};

        // Current selection
        std::vector<uint32_t> m_selectedUnits;

        // Per-unit command queues, ordered by unit id so processing order never depends on hashing
        std::map<uint32_t, std::vector<UnitCommand>> m_commandQueues;
    };

} // namespace RTS
