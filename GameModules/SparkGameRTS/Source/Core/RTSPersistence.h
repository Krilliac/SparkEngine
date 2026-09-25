/**
 * @file RTSPersistence.h
 * @brief Deterministic, validated full-state snapshot codec for an RTS skirmish.
 *
 * A snapshot holds everything RTSSkirmishSimulation::Step reads or writes: units, buildings, the economy and
 * resource nodes, the never-reused id counters and harvest timer, every command queue and the selection, the match
 * lifecycle and eliminations, each faction's fog grid (explored history cannot be rebuilt from units), and the sim
 * tick (which also fixes the AI decision phase). Loading a snapshot therefore resumes a skirmish bit-identically.
 *
 * Floats are written as their IEEE-754 bit patterns so a round trip is exact for every value, and the decoder
 * rejects any other format version, truncated input, trailing data, and out-of-range values without touching its
 * output.
 */

#pragma once

#include "Building/RTSBuildingSystem.h"
#include "Command/RTSCommandSystem.h"
#include "FogOfWar/RTSFogOfWarSystem.h"
#include "Match/RTSMatchSystem.h"
#include "Resource/RTSResourceSystem.h"
#include "Unit/RTSUnitSystem.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RTS
{
    class RTSSkirmishSimulation;
    struct RTSSkirmishSystems;

    struct RTSPersistenceSnapshot
    {
        // Gameplay records
        std::vector<UnitData> units;
        std::vector<BuildingData> buildings;
        std::vector<std::pair<RTSFaction, PlayerResources>> players;
        std::vector<ResourceNode> resourceNodes;

        // Hidden simulation state that decides future ids and harvest timing
        uint32_t nextUnitId = 1;
        uint32_t nextBuildingId = 1;
        uint32_t nextNodeId = 1;
        float gatherTimer = 0.0f;

        // Orders, match lifecycle, fog of war, and the fixed-step clock
        std::map<uint32_t, std::vector<UnitCommand>> commandQueues;
        std::vector<uint32_t> selection;
        RTSMatchSnapshot match;
        std::vector<FogGrid> fog; ///< One grid per faction, indexed by RTSFaction
        uint64_t tick = 0;
    };

    class RTSPersistence
    {
      public:
        /// Save custom-state key. Version 1 (records only) cannot resume a skirmish and is rejected.
        static constexpr std::string_view StateKey = "SparkGameRTS.match.v2";
        static constexpr std::string_view LegacyStateKeyV1 = "SparkGameRTS.match.v1";

        static constexpr size_t MAX_RECORDS = 10000;      ///< Per-section limit for units, buildings, nodes, orders
        static constexpr size_t MAX_PRODUCTION_QUEUE = 5; ///< Matches RTSBuildingSystem's queue limit
        static constexpr size_t MAX_NODE_WORKERS = 64;    ///< Decoder bound; nodes accept at most maxWorkers

        static bool IsValidSlotName(std::string_view slotName);

        /**
         * @brief Capture only the gameplay records (units, buildings, economy, nodes) in canonical id order.
         *
         * Id counters and the harvest timer are captured too; every other field keeps its default.
         */
        static RTSPersistenceSnapshot Capture(const RTSUnitSystem& units, const RTSBuildingSystem& buildings,
                                              const RTSResourceSystem& resources);

        /**
         * @brief Capture the complete skirmish state the simulation drives.
         * @pre Every pointer in @p systems is non-null.
         */
        static RTSPersistenceSnapshot Capture(const RTSSkirmishSystems& systems,
                                              const RTSSkirmishSimulation& simulation);

        /** @brief Check every bound and cross-reference Apply and the restoring systems rely on. */
        static bool Validate(const RTSPersistenceSnapshot& snapshot, std::string& error);

        /** @return The encoded snapshot, or an empty string (with @p error set) if it does not validate. */
        static std::string Serialize(const RTSPersistenceSnapshot& snapshot, std::string& error);
        static std::string Serialize(const RTSPersistenceSnapshot& snapshot);

        /** @brief Decode and validate; @p outSnapshot is replaced only on success. */
        static bool Deserialize(std::string_view text, RTSPersistenceSnapshot& outSnapshot, std::string& error);

        /**
         * @brief Restore every system and resume the simulation clock at the saved tick.
         *
         * An invalid snapshot is rejected by Validate before anything changes. Validate mirrors every check the
         * restoring systems make, so a validated snapshot is accepted by all of them; should the two ever drift,
         * Apply rolls each system back to its pre-call state and reports whether that rollback itself succeeded.
         * @pre Every pointer in @p systems is non-null.
         */
        static bool Apply(const RTSPersistenceSnapshot& snapshot, const RTSSkirmishSystems& systems,
                          RTSSkirmishSimulation& simulation, std::string& error);
    };
} // namespace RTS
