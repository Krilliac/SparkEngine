/**
 * @file TFUnlockTree.h
 * @brief Data-driven-in-code weapon/vehicle unlock tree (rank- and flux-gated).
 *
 * OWNERSHIP: progression lane (with TFProgressionSystem.h/.cpp and
 * Persistence/**). The table below is the single source of truth for what is
 * lockable; balance lives HERE, not scattered across systems.
 *
 * Semantics (enforced by TFProgressionSystem, the only mutator):
 *  - An item whose weapon key / vehicle id has NO entry in the tree is part of
 *    the default kit and is always available (faction rifles, pistols, knife,
 *    tools, Drifter).
 *  - fluxCost == 0: the unlock is auto-granted the moment the player's rank
 *    reaches requiredRank (and the prereq, if any, is satisfied). Nothing to
 *    buy, nothing stored.
 *  - fluxCost > 0: the unlock must be purchased once via
 *    TFProgressionSystem::ServerTryUnlock (rank + prereq gated, spends from
 *    the flux wallet) and is then persisted per character in TFDatabase.
 *  - Vehicle unlocks are one-time ACCESS gates; the per-spawn
 *    VehicleDef::fluxCost from vehicles.json still applies on every pull.
 *
 * Keys are durable (persisted in the character "unlocks" array), so never
 * rename an existing key — add new entries instead.
 */
#pragma once

#include "Core/TFTypes.h"

#include <string_view>
#include <vector>

namespace Terrafront
{

    enum class TFUnlockKind : uint8_t
    {
        Weapon = 0,
        Vehicle,
        Suit, ///< loadout-depth wave: Assets/MMOFPS/Data/suits.json passive (itemKey holds the suit key)
    };

    struct TFUnlockDef
    {
        const char* key; ///< durable unlock key (persisted; never rename)
        TFUnlockKind kind;
        const char* weaponKey; ///< weapons.json key (Weapon kind, incl. smoke/flash grenade
                               ///< choices); "" for vehicles/suits
        VehicleId vehicle;     ///< target vehicle (Vehicle kind); None otherwise
        uint16_t requiredRank; ///< minimum rank (1 == no rank gate)
        uint32_t fluxCost;     ///< one-time purchase price; 0 == auto-granted at rank
        const char* prereq;    ///< prerequisite unlock key; nullptr == root node
        const char* name;      ///< display name for HUD/directives UI
        const char* itemKey = ""; ///< loadout-depth wave: suits.json key (Suit kind only)
    };

    namespace TFUnlockTree
    {

        /// The whole tree, for UI enumeration (stable order: weapons then vehicles).
        const std::vector<TFUnlockDef>& All();

        /// nullptr if the unlock key is unknown.
        const TFUnlockDef* Find(std::string_view unlockKey);

        /// nullptr if this weapons.json key is not gated (== default kit).
        const TFUnlockDef* FindByWeaponKey(std::string_view weaponKey);

        /// nullptr if this vehicle is not gated (== always available).
        const TFUnlockDef* FindByVehicle(VehicleId vehicle);

        /// loadout-depth wave: nullptr if this suits.json key is not gated (==
        /// default kit; overshield ships with no tree entry, see TFUnlockTree.cpp).
        const TFUnlockDef* FindBySuitKey(std::string_view suitKey);

    } // namespace TFUnlockTree

} // namespace Terrafront
