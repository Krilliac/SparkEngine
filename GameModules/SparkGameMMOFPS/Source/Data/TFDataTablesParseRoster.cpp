/**
 * @file TFDataTablesParseRoster.cpp
 * @brief Roster-table parsers (factions/weapons/classes/vehicles) with the
 *        same fail-loud validation as before: unique ids, known slot/kind
 *        strings, complete class roster. Split from TFDataTables.cpp; the
 *        shared JSON accessors live in TFDataTablesInternal.h, the world
 *        tables in TFDataTablesParseWorld.cpp.
 */
#include "Data/TFDataTablesInternal.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Terrafront
{
    namespace DataTablesDetail
    {

        namespace
        {

            bool GetColor3(const Value& o, const char* k, float out[3])
            {
                if (!o.HasKey(k) || !o[k].IsArray() || o[k].Size() != 3)
                    return false;
                for (size_t i = 0; i < 3; ++i)
                    out[i] = static_cast<float>(o[k][i].AsNumber(0.5));
                return true;
            }

            bool ParseClassName(const std::string& s, ClassId& out)
            {
                if (s == "Ghost")
                    out = ClassId::Ghost;
                else if (s == "Striker")
                    out = ClassId::Striker;
                else if (s == "Medtech")
                    out = ClassId::Medtech;
                else if (s == "Fabricator")
                    out = ClassId::Fabricator;
                else if (s == "Bulwark")
                    out = ClassId::Bulwark;
                else if (s == "Colossus")
                    out = ClassId::Colossus;
                else
                    return false;
                return true;
            }

            bool ParseVehicleName(const std::string& s, VehicleId& out)
            {
                if (s == "Drifter")
                    out = VehicleId::Drifter;
                else if (s == "Aegis")
                    out = VehicleId::Aegis;
                else if (s == "Ravager")
                    out = VehicleId::Ravager;
                else if (s == "Vulture")
                    out = VehicleId::Vulture;
                else
                    return false;
                return true;
            }

        } // namespace

        bool ParseFactions(const Value& root, std::vector<FactionDef>& out, std::string& err)
        {
            const Value& arr = root["factions"];
            if (!arr.IsArray() || arr.Size() == 0)
            {
                err = "factions.json: missing 'factions' array";
                return false;
            }

            std::set<uint8_t> seen;
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const Value& o = arr[i];
                FactionDef f;
                int id = GetInt(o, "id", 0);
                if (id <= 0 || id >= static_cast<int>(FactionId::COUNT))
                {
                    err = "factions.json: bad faction id " + std::to_string(id);
                    return false;
                }
                if (!seen.insert(static_cast<uint8_t>(id)).second)
                {
                    err = "factions.json: duplicate faction id " + std::to_string(id);
                    return false;
                }
                f.id = static_cast<FactionId>(id);
                f.tag = GetStr(o, "tag");
                f.name = GetStr(o, "name");
                f.blurb = GetStr(o, "blurb");
                GetColor3(o, "color", f.color);
                GetColor3(o, "colorSecondary", f.colorSec);
                const Value& t = o["traits"];
                if (!t.IsObject())
                {
                    err = "factions.json: faction '" + f.tag + "' missing traits";
                    return false;
                }
                f.rofMult = GetNum(t, "rofMult", 1.0f);
                f.damageMult = GetNum(t, "damageMult", 1.0f);
                f.reloadMult = GetNum(t, "reloadMult", 1.0f);
                f.projGravityMult = GetNum(t, "projGravityMult", 1.0f);
                f.shieldRegenDelaySec = GetNum(t, "shieldRegenDelaySec", 6.0f);
                f.structureMaterial = GetStr(o, "structureMaterial", "Assets/Materials/MMOFPS/Structure_Concrete.json");
                out.push_back(std::move(f));
            }
            if (seen.size() != 3)
            {
                err = "factions.json: expected exactly 3 factions (MRA/AUC/HLX)";
                return false;
            }
            return true;
        }

        bool ParseWeapons(const Value& root, std::vector<WeaponDef>& out, std::string& err)
        {
            const Value& arr = root["weapons"];
            if (!arr.IsArray() || arr.Size() == 0)
            {
                err = "weapons.json: missing 'weapons' array";
                return false;
            }

            std::set<int> ids;
            std::set<std::string> keys;
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const Value& o = arr[i];
                WeaponDef w;
                int id = GetInt(o, "id", -1);
                w.key = GetStr(o, "key");
                if (id <= 0 || id >= static_cast<int>(kInvalidWeapon))
                {
                    err = "weapons.json: bad id for key '" + w.key + "'";
                    return false;
                }
                if (!ids.insert(id).second)
                {
                    err = "weapons.json: duplicate id " + std::to_string(id);
                    return false;
                }
                if (w.key.empty() || !keys.insert(w.key).second)
                {
                    err = "weapons.json: missing/duplicate key at id " + std::to_string(id);
                    return false;
                }
                w.id = static_cast<WeaponId>(id);
                w.name = GetStr(o, "name");
                if (!ParseFactionTag(GetStr(o, "faction", "ALL"), w.faction))
                {
                    err = "weapons.json: '" + w.key + "': unknown faction '" + GetStr(o, "faction") + "'";
                    return false;
                }
                w.slot = GetStr(o, "slot");
                w.kind = GetStr(o, "kind");
                if (!IsOneOf(w.slot, {"rifle", "carbine", "lmg", "sniper", "pistol", "shotgun", "launcher", "melee",
                                      "tool", "colossus_autocannon", "vehicle_main", "vehicle_turret"}))
                {
                    err = "weapons.json: '" + w.key + "': unknown slot '" + w.slot + "'";
                    return false;
                }
                if (!IsOneOf(w.kind, {"hitscan", "projectile", "melee", "beam"}))
                {
                    err = "weapons.json: '" + w.key + "': unknown kind '" + w.kind + "'";
                    return false;
                }
                w.damage = GetNum(o, "damage", w.damage);
                w.headshotMult = GetNum(o, "headshotMult", w.headshotMult);
                w.rofRpm = GetNum(o, "rofRpm", w.rofRpm);
                w.magSize = GetInt(o, "magSize", w.magSize);
                w.reserve = GetInt(o, "reserve", w.reserve);
                w.reloadSec = GetNum(o, "reloadSec", w.reloadSec);
                w.adsSec = GetNum(o, "adsSec", w.adsSec);
                w.reloadPerShell = GetBool(o, "reloadPerShell", w.reloadPerShell);
                w.spreadHipDeg = GetNum(o, "spreadHipDeg", w.spreadHipDeg);
                w.spreadAdsDeg = GetNum(o, "spreadAdsDeg", w.spreadAdsDeg);
                w.recoilVert = GetNum(o, "recoilVert", w.recoilVert);
                w.recoilHoriz = GetNum(o, "recoilHoriz", w.recoilHoriz);
                w.falloffStartM = GetNum(o, "falloffStartM", w.falloffStartM);
                w.falloffEndM = GetNum(o, "falloffEndM", w.falloffEndM);
                w.minDamage = GetNum(o, "minDamage", w.minDamage);
                w.projSpeed = GetNum(o, "projSpeed", w.projSpeed);
                w.gravity = GetNum(o, "gravity", w.gravity);
                w.pellets = GetInt(o, "pellets", w.pellets);
                w.rangeM = GetNum(o, "rangeM", w.rangeM);
                w.splashRadiusM = GetNum(o, "splashRadiusM", w.splashRadiusM);
                w.splashDamage = GetNum(o, "splashDamage", w.splashDamage);
                w.vsVehicleMult = GetNum(o, "vsVehicleMult", w.vsVehicleMult);
                w.healsInfantry = GetBool(o, "healsInfantry", w.healsInfantry);
                w.healsVehicles = GetBool(o, "healsVehicles", w.healsVehicles);
                w.canRevive = GetBool(o, "canRevive", w.canRevive);
                w.model = GetStr(o, "model");
                w.audioFire = GetStr(o, "audioFire");
                w.audioReload = GetStr(o, "audioReload");
                if (o.HasKey("audioFireVariants") && o["audioFireVariants"].IsArray())
                {
                    const Value& av = o["audioFireVariants"];
                    for (size_t i = 0; i < av.Size(); ++i)
                    {
                        std::string p = av[i].AsString("");
                        if (!p.empty())
                            w.audioFireVariants.push_back(std::move(p));
                    }
                }
                if (w.audioFireVariants.empty() && !w.audioFire.empty())
                    w.audioFireVariants.push_back(w.audioFire);
                if (w.pellets < 1)
                {
                    err = "weapons.json: '" + w.key + "': pellets must be >= 1";
                    return false;
                }
                out.push_back(std::move(w));
            }
            return true;
        }

        bool ParseClasses(const Value& root, std::vector<ClassDef>& out, std::string& err)
        {
            const Value& arr = root["classes"];
            if (!arr.IsArray() || arr.Size() == 0)
            {
                err = "classes.json: missing 'classes' array";
                return false;
            }

            std::set<uint8_t> seen;
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const Value& o = arr[i];
                ClassDef c;
                std::string idStr = GetStr(o, "id");
                if (!ParseClassName(idStr, c.id))
                {
                    err = "classes.json: unknown class id '" + idStr + "'";
                    return false;
                }
                if (!seen.insert(static_cast<uint8_t>(c.id)).second)
                {
                    err = "classes.json: duplicate class '" + idStr + "'";
                    return false;
                }
                c.name = GetStr(o, "name", idStr);
                c.role = GetStr(o, "role");
                c.health = GetNum(o, "health", c.health);
                c.shield = GetNum(o, "shield", c.shield);
                c.sprintSpeed = GetNum(o, "sprintSpeed", c.sprintSpeed);
                c.runSpeed = GetNum(o, "runSpeed", c.runSpeed);
                const Value& a = o["ability"];
                if (a.IsObject())
                {
                    c.ability.key = GetStr(a, "key");
                    c.ability.name = GetStr(a, "name");
                    c.ability.desc = GetStr(a, "desc");
                    c.ability.durationSec = GetNum(a, "durationSec", 0.0f);
                    c.ability.cooldownSec = GetNum(a, "cooldownSec", 0.0f);
                    c.ability.regenPerSec = GetNum(a, "regenPerSec", 0.0f);
                    c.ability.toggle = GetBool(a, "toggle", false);
                }
                const Value& prim = o["primaries"];
                if (!prim.IsArray() || prim.Size() == 0)
                {
                    err = "classes.json: class '" + idStr + "' missing 'primaries'";
                    return false;
                }
                for (size_t p = 0; p < prim.Size(); ++p)
                    c.primarySlots.push_back(prim[p].AsString(""));
                c.secondarySlot = GetStr(o, "secondary", "none");
                c.toolKey = GetStr(o, "tool", "none");
                c.grenades = GetInt(o, "grenades", 0);
                c.rocketLauncher = GetBool(o, "rocketLauncher", false);
                c.fluxCost = GetInt(o, "fluxCost", 0);
                c.noRegen = GetBool(o, "noRegen", false);
                c.mesh = GetStr(o, "mesh", ""); // per-class pawn body; empty falls back to presentation.pawnMesh
                out.push_back(std::move(c));
            }
            if (seen.size() != static_cast<size_t>(ClassId::COUNT))
            {
                err = "classes.json: expected all " + std::to_string(static_cast<int>(ClassId::COUNT)) + " classes";
                return false;
            }
            return true;
        }

        bool ParseVehicles(const Value& root, std::vector<VehicleDef>& out, std::string& err)
        {
            const Value& arr = root["vehicles"];
            if (!arr.IsArray() || arr.Size() == 0)
            {
                err = "vehicles.json: missing 'vehicles' array";
                return false;
            }

            std::set<uint8_t> seen;
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const Value& o = arr[i];
                VehicleDef v;
                std::string idStr = GetStr(o, "id");
                if (!ParseVehicleName(idStr, v.id))
                {
                    err = "vehicles.json: unknown vehicle id '" + idStr + "'";
                    return false;
                }
                if (!seen.insert(static_cast<uint8_t>(v.id)).second)
                {
                    err = "vehicles.json: duplicate vehicle '" + idStr + "'";
                    return false;
                }
                v.name = GetStr(o, "name", idStr);
                v.role = GetStr(o, "role");
                v.enabled = GetBool(o, "enabled", true);
                v.health = GetNum(o, "health", v.health);
                v.fluxCost = GetInt(o, "fluxCost", 0);
                v.topSpeed = GetNum(o, "topSpeed", v.topSpeed);
                v.accel = GetNum(o, "accel", v.accel);
                v.turnRate = GetNum(o, "turnRate", v.turnRate);
                const Value& seats = o["seats"];
                if (!seats.IsArray() || seats.Size() == 0)
                {
                    err = "vehicles.json: '" + idStr + "' has no seats";
                    return false;
                }
                for (size_t s = 0; s < seats.Size(); ++s)
                {
                    VehicleSeatDef seat;
                    seat.role = GetStr(seats[s], "role", "passenger");
                    seat.weaponKey = GetStr(seats[s], "weapon");
                    v.seats.push_back(std::move(seat));
                }
                if (o.HasKey("deploySpawn") && o["deploySpawn"].IsObject())
                {
                    const Value& d = o["deploySpawn"];
                    v.hasDeploySpawn = true;
                    v.deployRadiusM = GetNum(d, "radiusM", 0.0f);
                    v.deployRespawnSec = GetNum(d, "respawnSec", 0.0f);
                }
                v.model = GetStr(o, "model");
                v.audioEngine = GetStr(o, "audioEngine");
                v.explodeAudio = GetStr(o, "explodeAudio");
                v.turretMesh = GetStr(o, "turretMesh", "");
                v.deployMesh = GetStr(o, "deployMesh", "");
                if (o.HasKey("turretPivot") && o["turretPivot"].IsArray() && o["turretPivot"].Size() >= 3)
                {
                    const Value& tp = o["turretPivot"];
                    v.turretPivot[0] = static_cast<float>(tp[0].AsNumber(0.0));
                    v.turretPivot[1] = static_cast<float>(tp[1].AsNumber(0.0));
                    v.turretPivot[2] = static_cast<float>(tp[2].AsNumber(0.0));
                }
                out.push_back(std::move(v));
            }
            return true;
        }

    } // namespace DataTablesDetail
} // namespace Terrafront
