/**
 * @file OWSaveData.h
 * @brief Versioned snapshot for OpenWorld gameplay state.
 */

#pragma once

#include "Events/OWDynamicEventSystem.h"
#include "Exploration/OWExplorationSystem.h"
#include "Gathering/OWGatheringSystem.h"
#include "Player/OWPlayerSystem.h"
#include "Settlement/OWSettlementSystem.h"
#include "Wildlife/OWWildlifeSystem.h"

#include <cstdint>

namespace OpenWorld
{

    inline constexpr uint32_t kOpenWorldSaveVersion = 1;

    /// @brief All non-ECS state owned by the OpenWorld gameplay systems.
    struct OWGameSaveData
    {
        uint32_t version = kOpenWorldSaveVersion;
        PlayerSaveState player;
        ExplorationSaveState exploration;
        GatheringSaveState gathering;
        SettlementSaveState settlements;
        WildlifeSaveState wildlife;
        DynamicEventSaveState events;
    };

} // namespace OpenWorld
