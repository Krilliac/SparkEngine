/**
 * @file TFUnlockTree.cpp
 * @brief The unlock table (code data table) + linear lookups.
 *
 * The table is tiny (a dozen entries) and read-mostly, so lookups are linear
 * scans — no maps, no allocation beyond the one static vector.
 */
#include "Persistence/TFUnlockTree.h"

namespace Terrafront
{
    namespace TFUnlockTree
    {

        namespace
        {

            const std::vector<TFUnlockDef>& Table()
            {
                // key, kind, weaponKey, vehicle, requiredRank, fluxCost, prereq, name
                static const std::vector<TFUnlockDef> s_table = {
                    // --- faction carbines: free at rank 3 -------------------------------
                    {"wpn_mra_carbine", TFUnlockKind::Weapon, "mra_carbine", VehicleId::None, 3, 0, nullptr,
                     "Skirmisher Carbine"},
                    {"wpn_auc_carbine", TFUnlockKind::Weapon, "auc_carbine", VehicleId::None, 3, 0, nullptr,
                     "Prospect Carbine"},
                    {"wpn_hlx_carbine", TFUnlockKind::Weapon, "hlx_carbine", VehicleId::None, 3, 0, nullptr,
                     "Strand Carbine"},
                    // --- faction LMGs: free at rank 5 ------------------------------------
                    {"wpn_mra_lmg", TFUnlockKind::Weapon, "mra_lmg", VehicleId::None, 5, 0, nullptr, "Meridian LMG"},
                    {"wpn_auc_lmg", TFUnlockKind::Weapon, "auc_lmg", VehicleId::None, 5, 0, nullptr, "Aurum LMG"},
                    {"wpn_hlx_lmg", TFUnlockKind::Weapon, "hlx_lmg", VehicleId::None, 5, 0, nullptr, "Helix LMG"},
                    // --- faction snipers: rank 8 + 150 flux -----------------------------
                    {"wpn_mra_sniper", TFUnlockKind::Weapon, "mra_sniper", VehicleId::None, 8, 150, nullptr,
                     "Meridian Longrifle"},
                    {"wpn_auc_sniper", TFUnlockKind::Weapon, "auc_sniper", VehicleId::None, 8, 150, nullptr,
                     "Aurum Longrifle"},
                    {"wpn_hlx_sniper", TFUnlockKind::Weapon, "hlx_sniper", VehicleId::None, 8, 150, nullptr,
                     "Helix Lance"},
                    // --- common pool ------------------------------------------------------
                    {"wpn_np_shotgun", TFUnlockKind::Weapon, "np_shotgun", VehicleId::None, 4, 100, nullptr,
                     "Breacher Shotgun"},
                    {"wpn_np_launcher", TFUnlockKind::Weapon, "np_launcher", VehicleId::None, 10, 250, "wpn_np_shotgun",
                     "Siege Launcher"},
                    // --- vehicles (access gates; per-spawn fluxCost still applies) ------
                    {"veh_aegis", TFUnlockKind::Vehicle, "", VehicleId::Aegis, 6, 0, nullptr, "Aegis Transport"},
                    {"veh_ravager", TFUnlockKind::Vehicle, "", VehicleId::Ravager, 9, 200, nullptr, "Ravager Tank"},
                    {"veh_vulture", TFUnlockKind::Vehicle, "", VehicleId::Vulture, 15, 400, "veh_ravager",
                     "Vulture Gunship"},
                };
                return s_table;
            }

        } // namespace

        const std::vector<TFUnlockDef>& All()
        {
            return Table();
        }

        const TFUnlockDef* Find(std::string_view unlockKey)
        {
            for (const TFUnlockDef& d : Table())
                if (unlockKey == d.key)
                    return &d;
            return nullptr;
        }

        const TFUnlockDef* FindByWeaponKey(std::string_view weaponKey)
        {
            if (weaponKey.empty())
                return nullptr;
            for (const TFUnlockDef& d : Table())
                if (d.kind == TFUnlockKind::Weapon && weaponKey == d.weaponKey)
                    return &d;
            return nullptr;
        }

        const TFUnlockDef* FindByVehicle(VehicleId vehicle)
        {
            if (vehicle == VehicleId::None)
                return nullptr;
            for (const TFUnlockDef& d : Table())
                if (d.kind == TFUnlockKind::Vehicle && d.vehicle == vehicle)
                    return &d;
            return nullptr;
        }

    } // namespace TFUnlockTree
} // namespace Terrafront
