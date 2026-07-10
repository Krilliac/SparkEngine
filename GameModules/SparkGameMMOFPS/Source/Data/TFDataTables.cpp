/**
 * @file TFDataTables.cpp
 * @brief JSON data-table loaders (weapons/vehicles/classes/regions/factions).
 *
 * Loads all five tables from Assets/MMOFPS/Data/, validates them (unique ids,
 * known slot/kind/tier strings, conduit symmetry, complete initial ownership)
 * and fails LOUD: any error aborts the load with a clear log message and no
 * half-loaded state. Parsing goes through Spark::Json (JsonUtils.h), which is
 * backed by nlohmann/json when SPARK_HAS_NLOHMANN_JSON is available.
 */
#include "Data/TFDataTables.h"
#include "Game/TFDeployableTypes.h" // W6: extended DeployableKind constants
#include "Utils/LogMacros.h"
#include "Utils/JsonUtils.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        constexpr const char* kDataDir = "Assets/MMOFPS/Data/";

        using Spark::Json::Value;

        bool ReadFileText(const std::string& path, std::string& out, std::string& err)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f.is_open())
            {
                err = "cannot open '" + path + "'";
                return false;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            if (out.empty())
            {
                err = "'" + path + "' is empty";
                return false;
            }
            return true;
        }

        /// Load + parse one JSON file; false (with err) on any failure.
        bool LoadJsonFile(const std::string& file, Value& out, std::string& err)
        {
            std::string text;
            if (!ReadFileText(std::string(kDataDir) + file, text, err))
                return false;
            out = Spark::Json::Parse(text);
            if (!out.IsObject())
            {
                err = file + ": malformed JSON (parse returned non-object)";
                return false;
            }
            return true;
        }

        // Missing-key-tolerant accessors. Value::operator[] logs a warning on type
        // mismatch, so we pre-check HasKey to keep absent optional fields silent.
        float GetNum(const Value& o, const char* k, float def)
        {
            return o.HasKey(k) ? static_cast<float>(o[k].AsNumber(def)) : def;
        }
        int GetInt(const Value& o, const char* k, int def)
        {
            return o.HasKey(k) ? o[k].AsInt(def) : def;
        }
        bool GetBool(const Value& o, const char* k, bool def)
        {
            return o.HasKey(k) ? o[k].AsBool(def) : def;
        }
        std::string GetStr(const Value& o, const char* k, const std::string& def = {})
        {
            return o.HasKey(k) ? o[k].AsString(def) : def;
        }

        bool GetColor3(const Value& o, const char* k, float out[3])
        {
            if (!o.HasKey(k) || !o[k].IsArray() || o[k].Size() != 3)
                return false;
            for (size_t i = 0; i < 3; ++i)
                out[i] = static_cast<float>(o[k][i].AsNumber(0.5));
            return true;
        }

        bool ParseFactionTag(const std::string& tag, FactionId& out)
        {
            if (tag == "MRA")
                out = FactionId::MRA;
            else if (tag == "AUC")
                out = FactionId::AUC;
            else if (tag == "HLX")
                out = FactionId::HLX;
            else if (tag == "ALL" || tag.empty() || tag == "None")
                out = FactionId::None;
            else
                return false;
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

        bool IsOneOf(const std::string& s, std::initializer_list<const char*> opts)
        {
            for (const char* o : opts)
                if (s == o)
                    return true;
            return false;
        }

        // -------------------------------------------------------------------------

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

        bool ParseRegions(const Value& root, ContinentDef& out, std::string& err)
        {
            const Value& cont = root["continent"];
            if (!cont.IsObject())
            {
                err = "regions.json: missing 'continent' object";
                return false;
            }
            out.name = GetStr(cont, "name", "Cindral Wastes");
            out.sizeM = GetNum(cont, "sizeM", 4096.0f);
            out.scene = GetStr(cont, "scene");
            out.fluxTickSec = GetNum(cont, "fluxTickSec", 60.0f);

            const Value& arr = root["regions"];
            if (!arr.IsArray() || arr.Size() == 0)
            {
                err = "regions.json: missing 'regions' array";
                return false;
            }
            if (arr.Size() > kMaxRegions)
            {
                err = "regions.json: too many regions";
                return false;
            }

            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const Value& o = arr[i];
                RegionDef r;
                int id = GetInt(o, "id", -1);
                r.key = GetStr(o, "key");
                if (id < 0 || id >= static_cast<int>(kMaxRegions))
                {
                    err = "regions.json: bad region id for key '" + r.key + "'";
                    return false;
                }
                r.id = static_cast<RegionId>(id);
                r.name = GetStr(o, "name");
                r.tier = GetStr(o, "tier");
                if (!IsOneOf(r.tier, {"skyanchor", "outpost", "fort", "facility"}))
                {
                    err = "regions.json: '" + r.key + "': unknown tier '" + r.tier + "'";
                    return false;
                }
                if (o.HasKey("homeFaction") && !ParseFactionTag(GetStr(o, "homeFaction"), r.homeFaction))
                {
                    err = "regions.json: '" + r.key + "': bad homeFaction";
                    return false;
                }
                if (r.tier == "skyanchor" && r.homeFaction == FactionId::None)
                {
                    err = "regions.json: skyanchor '" + r.key + "' needs a homeFaction";
                    return false;
                }
                if (o.HasKey("hex") && o["hex"].IsArray() && o["hex"].Size() == 2)
                {
                    r.hexQ = o["hex"][0].AsInt(0);
                    r.hexR = o["hex"][1].AsInt(0);
                }
                if (!o.HasKey("center") || !o["center"].IsArray() || o["center"].Size() != 2)
                {
                    err = "regions.json: '" + r.key + "': missing center [x,z]";
                    return false;
                }
                r.centerX = static_cast<float>(o["center"][0].AsNumber(0));
                r.centerZ = static_cast<float>(o["center"][1].AsNumber(0));
                r.captureSec = GetNum(o, "captureSec", 60.0f);
                r.fluxPerTick = GetInt(o, "fluxPerTick", 0);

                auto parseXZList = [&](const char* k, std::vector<std::array<float, 2>>& dst) -> bool
                {
                    if (!o.HasKey(k) || !o[k].IsArray())
                        return true; // absent == empty
                    for (size_t p = 0; p < o[k].Size(); ++p)
                    {
                        const Value& pt = o[k][p];
                        if (!pt.IsArray() || pt.Size() != 2)
                            return false;
                        dst.push_back({static_cast<float>(pt[0].AsNumber(0)), static_cast<float>(pt[1].AsNumber(0))});
                    }
                    return true;
                };
                if (!parseXZList("capturePoints", r.capturePoints) || !parseXZList("spawns", r.spawns))
                {
                    err = "regions.json: '" + r.key + "': malformed capturePoints/spawns";
                    return false;
                }
                if (r.spawns.empty())
                {
                    err = "regions.json: '" + r.key + "' has no spawns";
                    return false;
                }
                if (r.tier != "skyanchor" && r.capturePoints.empty())
                {
                    err = "regions.json: capturable region '" + r.key + "' has no capturePoints";
                    return false;
                }
                if (r.capturePoints.size() > kMaxCapturePoints)
                {
                    err = "regions.json: '" + r.key + "' exceeds kMaxCapturePoints";
                    return false;
                }
                if (o.HasKey("vehicleTerminal") && o["vehicleTerminal"].IsArray() && o["vehicleTerminal"].Size() == 2)
                {
                    r.vehicleTerminal = std::array<float, 2>{static_cast<float>(o["vehicleTerminal"][0].AsNumber(0)),
                                                             static_cast<float>(o["vehicleTerminal"][1].AsNumber(0))};
                }
                out.regions.push_back(std::move(r));
            }

            // Region ids must be contiguous [0..N) so RegionId can index vectors.
            std::sort(out.regions.begin(), out.regions.end(),
                      [](const RegionDef& a, const RegionDef& b) { return a.id < b.id; });
            for (size_t i = 0; i < out.regions.size(); ++i)
            {
                if (out.regions[i].id != static_cast<RegionId>(i))
                {
                    err = "regions.json: region ids must be contiguous starting at 0";
                    return false;
                }
            }
            const size_t count = out.regions.size();

            // Conduits -> symmetric neighbor lists.
            const Value& conduits = root["conduits"];
            if (!conduits.IsArray() || conduits.Size() == 0)
            {
                err = "regions.json: missing 'conduits' array";
                return false;
            }
            for (size_t i = 0; i < conduits.Size(); ++i)
            {
                const Value& c = conduits[i];
                if (!c.IsArray() || c.Size() != 2)
                {
                    err = "regions.json: conduit " + std::to_string(i) + " malformed";
                    return false;
                }
                int a = c[0].AsInt(-1), b = c[1].AsInt(-1);
                if (a < 0 || b < 0 || a >= static_cast<int>(count) || b >= static_cast<int>(count) || a == b)
                {
                    err = "regions.json: conduit [" + std::to_string(a) + "," + std::to_string(b) + "] out of range";
                    return false;
                }
                auto link = [](RegionDef& r, RegionId n)
                {
                    if (std::find(r.neighbors.begin(), r.neighbors.end(), n) == r.neighbors.end())
                        r.neighbors.push_back(n);
                };
                link(out.regions[static_cast<size_t>(a)], static_cast<RegionId>(b));
                link(out.regions[static_cast<size_t>(b)], static_cast<RegionId>(a));
            }

            // Initial ownership: every region assigned exactly once.
            out.initialOwner.assign(count, FactionId::None);
            const Value& own = root["initialOwnership"];
            if (!own.IsObject())
            {
                err = "regions.json: missing 'initialOwnership'";
                return false;
            }
            std::vector<bool> assigned(count, false);
            const std::pair<const char*, FactionId> lists[] = {{"MRA", FactionId::MRA},
                                                               {"AUC", FactionId::AUC},
                                                               {"HLX", FactionId::HLX},
                                                               {"neutral", FactionId::None}};
            for (const auto& [k, fac] : lists)
            {
                if (!own.HasKey(k) || !own[k].IsArray())
                {
                    err = std::string("regions.json: initialOwnership missing '") + k + "'";
                    return false;
                }
                for (size_t i = 0; i < own[k].Size(); ++i)
                {
                    int id = own[k][i].AsInt(-1);
                    if (id < 0 || id >= static_cast<int>(count))
                    {
                        err = std::string("regions.json: initialOwnership.") + k + " has bad region id";
                        return false;
                    }
                    if (assigned[static_cast<size_t>(id)])
                    {
                        err = "regions.json: region " + std::to_string(id) + " assigned twice in initialOwnership";
                        return false;
                    }
                    assigned[static_cast<size_t>(id)] = true;
                    out.initialOwner[static_cast<size_t>(id)] = fac;
                }
            }
            for (size_t i = 0; i < count; ++i)
            {
                if (!assigned[i])
                {
                    err = "regions.json: region " + std::to_string(i) + " missing from initialOwnership";
                    return false;
                }
            }
            return true;
        }

        bool ParsePresentation(const Value& root, WorldPresentationDef& out, std::string& err)
        {
            const Value& pres = root["presentation"];
            if (!pres.IsObject())
            {
                err = "presentation.json: missing 'presentation' object";
                return false;
            }

            const Value& sky = pres["skybox"];
            if (!sky.IsObject() || !sky["faceTex"].IsArray() || sky["faceTex"].Size() != 6)
            {
                err = "presentation.json: skybox.faceTex must have exactly 6 entries";
                return false;
            }
            for (size_t i = 0; i < 6; ++i)
                out.skybox.faceTex[i] = sky["faceTex"][i].AsString(out.skybox.faceTex[i]);
            out.skybox.scale = GetNum(sky, "scale", out.skybox.scale);
            if (sky["tint"].IsArray() && sky["tint"].Size() == 4)
            {
                for (size_t i = 0; i < 4; ++i)
                    out.skybox.tint[i] = static_cast<float>(sky["tint"][i].AsNumber(out.skybox.tint[i]));
            }

            const Value& terr = pres["terrain"];
            if (terr.IsObject())
            {
                out.terrain.texture = GetStr(terr, "texture", out.terrain.texture);
                out.terrain.uvTiles = GetNum(terr, "uvTiles", out.terrain.uvTiles);
            }

            const Value& amb = pres["ambient"];
            if (amb.IsObject())
            {
                out.ambient.path = GetStr(amb, "path", out.ambient.path);
                out.ambient.volume = GetNum(amb, "volume", out.ambient.volume);
            }

            const Value& vm = pres["viewmodel"];
            if (vm.IsObject())
            {
                if (vm["recenter"].IsArray() && vm["recenter"].Size() == 3)
                    for (size_t i = 0; i < 3; ++i)
                        out.viewmodel.recenter[i] =
                            static_cast<float>(vm["recenter"][i].AsNumber(out.viewmodel.recenter[i]));
                out.viewmodel.scale = GetNum(vm, "scale", out.viewmodel.scale);
                out.viewmodel.rotationYRad = GetNum(vm, "rotationYRad", out.viewmodel.rotationYRad);
                if (vm["place"].IsArray() && vm["place"].Size() == 3)
                    for (size_t i = 0; i < 3; ++i)
                        out.viewmodel.place[i] = static_cast<float>(vm["place"][i].AsNumber(out.viewmodel.place[i]));
                if (vm["gunmetal"].IsArray() && vm["gunmetal"].Size() == 4)
                    for (size_t i = 0; i < 4; ++i)
                        out.viewmodel.gunmetal[i] =
                            static_cast<float>(vm["gunmetal"][i].AsNumber(out.viewmodel.gunmetal[i]));
            }

            const Value& fx = pres["muzzleFx"];
            if (fx.IsObject())
            {
                out.muzzleFx.muzzleForwardM = GetNum(fx, "muzzleForwardM", out.muzzleFx.muzzleForwardM);
                out.muzzleFx.muzzleRightM = GetNum(fx, "muzzleRightM", out.muzzleFx.muzzleRightM);
                out.muzzleFx.muzzleUpM = GetNum(fx, "muzzleUpM", out.muzzleFx.muzzleUpM);
                out.muzzleFx.tracerLenM = GetNum(fx, "tracerLenM", out.muzzleFx.tracerLenM);
                out.muzzleFx.tracerThickM = GetNum(fx, "tracerThickM", out.muzzleFx.tracerThickM);
                out.muzzleFx.tracerLifeSec = GetNum(fx, "tracerLifeSec", out.muzzleFx.tracerLifeSec);
                out.muzzleFx.flashLifeSec = GetNum(fx, "flashLifeSec", out.muzzleFx.flashLifeSec);
                if (fx["tracerColor"].IsArray() && fx["tracerColor"].Size() == 4)
                    for (size_t i = 0; i < 4; ++i)
                        out.muzzleFx.tracerColor[i] =
                            static_cast<float>(fx["tracerColor"][i].AsNumber(out.muzzleFx.tracerColor[i]));
                if (fx["flashColor"].IsArray() && fx["flashColor"].Size() == 4)
                    for (size_t i = 0; i < 4; ++i)
                        out.muzzleFx.flashColor[i] =
                            static_cast<float>(fx["flashColor"][i].AsNumber(out.muzzleFx.flashColor[i]));
                if (fx["flashScale"].IsArray() && fx["flashScale"].Size() == 3)
                    for (size_t i = 0; i < 3; ++i)
                        out.muzzleFx.flashScale[i] =
                            static_cast<float>(fx["flashScale"][i].AsNumber(out.muzzleFx.flashScale[i]));
            }

            out.pawnMesh = GetStr(pres, "pawnMesh", out.pawnMesh);
            return true;
        }

        bool ParseDeployables(const Value& root, std::vector<DeployableVisualDef>& out, std::string& err)
        {
            const Value& arr = root["deployables"];
            if (!arr.IsArray() || arr.Size() == 0)
            {
                err = "deployables.json: missing 'deployables' array";
                return false;
            }

            auto parseKind = [](const std::string& s, DeployableKind& k) -> bool
            {
                if (s == "FabTurret")
                    k = DeployableKind::FabTurret;
                else if (s == "FabAmmoPack")
                    k = DeployableKind::FabAmmoPack;
                else if (s == "MedBeacon")
                    k = DeployableKind::MedBeacon;
                // W6 extended kinds (Game/TFDeployableTypes.h; values continue the enum).
                else if (s == "ResupplyStation")
                    k = kDeployResupplyStation;
                else if (s == "AVTurret")
                    k = kDeployAVTurret;
                else if (s == "ShieldWall")
                    k = kDeployShieldWall;
                else
                    return false;
                return true;
            };

            std::set<uint8_t> seen;
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const Value& o = arr[i];
                DeployableVisualDef d;
                const std::string idStr = GetStr(o, "id");
                if (!parseKind(idStr, d.id))
                {
                    err = "deployables.json: unknown deployable id '" + idStr + "'";
                    return false;
                }
                if (!seen.insert(static_cast<uint8_t>(d.id)).second)
                {
                    err = "deployables.json: duplicate deployable id '" + idStr + "'";
                    return false;
                }
                d.model = GetStr(o, "model");
                if (d.model.empty())
                {
                    err = "deployables.json: '" + idStr + "' missing model";
                    return false;
                }
                if (o["scale"].IsArray() && o["scale"].Size() == 3)
                {
                    for (size_t s = 0; s < 3; ++s)
                        d.scale[s] = static_cast<float>(o["scale"][s].AsNumber(1.0));
                }
                out.push_back(std::move(d));
            }
            // The 3 W3 base kinds are mandatory (extended W6 rows are optional — the
            // deployable system falls back to a scaled alias of a base row without them).
            for (uint8_t k = 0; k < 3; ++k)
            {
                if (!seen.contains(k))
                {
                    err = "deployables.json: expected all 3 base deployable kinds (FabTurret/FabAmmoPack/MedBeacon)";
                    return false;
                }
            }
            return true;
        }

    } // namespace

    // ===========================================================================

    TFDataTables::TFDataTables() = default;
    TFDataTables::~TFDataTables()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFDataTables::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;

        std::string err;
        if (!LoadAllInternal(err))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Data table load FAILED: %s", err.c_str());
            return false; // fail loud - no half-loaded state
        }
        SPARK_LOG_INFO(
            Spark::LogCategory::Game,
            "[TF] Data tables loaded: %zu factions, %zu weapons, %zu classes, %zu vehicles, %zu regions (%s)",
            m_factions.size(), m_weapons.size(), m_classes.size(), m_vehicles.size(), m_continent.regions.size(),
            m_continent.name.c_str());
        return true;
    }

    void TFDataTables::Update(float deltaTime)
    {
        (void)deltaTime;
    }
    void TFDataTables::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFDataTables::Shutdown()
    {
        m_factions.clear();
        m_weapons.clear();
        m_classes.clear();
        m_vehicles.clear();
        m_continent = {};
        m_presentation = {};
        m_deployableVisuals.clear();
        m_loaded = false;
        m_initialized = false;
    }

    bool TFDataTables::ReloadAll()
    {
        std::string err;
        if (!LoadAllInternal(err))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] tf_reload_data FAILED (previous tables kept): %s",
                            err.c_str());
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Data tables reloaded from disk");
        if (m_events)
            m_events->Fire(EvDataReloaded{});
        return true;
    }

    bool TFDataTables::LoadAllInternal(std::string& outError)
    {
        // Parse everything into locals first; commit only if ALL tables validate,
        // so a bad reload never leaves partially updated state.
        Value jFactions, jWeapons, jClasses, jVehicles, jRegions, jPresentation, jDeployables;
        if (!LoadJsonFile("factions.json", jFactions, outError))
            return false;
        if (!LoadJsonFile("weapons.json", jWeapons, outError))
            return false;
        if (!LoadJsonFile("classes.json", jClasses, outError))
            return false;
        if (!LoadJsonFile("vehicles.json", jVehicles, outError))
            return false;
        if (!LoadJsonFile("regions.json", jRegions, outError))
            return false;
        if (!LoadJsonFile("presentation.json", jPresentation, outError))
            return false;
        if (!LoadJsonFile("deployables.json", jDeployables, outError))
            return false;

        std::vector<FactionDef> factions;
        std::vector<WeaponDef> weapons;
        std::vector<ClassDef> classes;
        std::vector<VehicleDef> vehicles;
        ContinentDef continent;
        WorldPresentationDef presentation;
        std::vector<DeployableVisualDef> deployableVisuals;

        if (!ParseFactions(jFactions, factions, outError))
            return false;
        if (!ParseWeapons(jWeapons, weapons, outError))
            return false;
        if (!ParseClasses(jClasses, classes, outError))
            return false;
        if (!ParseVehicles(jVehicles, vehicles, outError))
            return false;
        if (!ParseRegions(jRegions, continent, outError))
            return false;
        if (!ParsePresentation(jPresentation, presentation, outError))
            return false;
        if (!ParseDeployables(jDeployables, deployableVisuals, outError))
            return false;

        // Cross-table check: vehicle seat weapons must reference known weapon keys.
        for (const VehicleDef& v : vehicles)
        {
            for (const VehicleSeatDef& s : v.seats)
            {
                if (s.weaponKey.empty())
                    continue;
                bool found = std::any_of(weapons.begin(), weapons.end(),
                                         [&](const WeaponDef& w) { return w.key == s.weaponKey; });
                if (!found)
                {
                    outError = "vehicles.json: '" + v.name + "' seat weapon '" + s.weaponKey + "' not in weapons.json";
                    return false;
                }
            }
        }

        m_factions = std::move(factions);
        m_weapons = std::move(weapons);
        m_classes = std::move(classes);
        m_vehicles = std::move(vehicles);
        m_continent = std::move(continent);
        m_presentation = std::move(presentation);
        m_deployableVisuals = std::move(deployableVisuals);
        m_loaded = true;
        return true;
    }

    // --- typed accessors --------------------------------------------------------

    const FactionDef* TFDataTables::GetFaction(FactionId f) const
    {
        for (const FactionDef& d : m_factions)
            if (d.id == f)
                return &d;
        return nullptr;
    }

    const WeaponDef* TFDataTables::GetWeapon(WeaponId id) const
    {
        for (const WeaponDef& w : m_weapons)
            if (w.id == id)
                return &w;
        return nullptr;
    }

    const WeaponDef* TFDataTables::GetWeaponByKey(const std::string& key) const
    {
        for (const WeaponDef& w : m_weapons)
            if (w.key == key)
                return &w;
        return nullptr;
    }

    const ClassDef* TFDataTables::GetClass(ClassId id) const
    {
        for (const ClassDef& c : m_classes)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    const ClassDef* TFDataTables::GetClassByName(const std::string& name) const
    {
        for (const ClassDef& c : m_classes)
            if (c.name == name)
                return &c;
        return nullptr;
    }

    const VehicleDef* TFDataTables::GetVehicle(VehicleId id) const
    {
        for (const VehicleDef& v : m_vehicles)
            if (v.id == id)
                return &v;
        return nullptr;
    }

    const DeployableVisualDef* TFDataTables::GetDeployableVisual(DeployableKind kind) const
    {
        for (const DeployableVisualDef& d : m_deployableVisuals)
            if (d.id == kind)
                return &d;
        return nullptr;
    }

    const RegionDef* TFDataTables::GetRegion(RegionId id) const
    {
        // ids are validated contiguous [0..N) at load time
        if (id >= m_continent.regions.size())
            return nullptr;
        return &m_continent.regions[id];
    }

    WeaponDef TFDataTables::ResolveWeapon(WeaponId id, FactionId f) const
    {
        const WeaponDef* base = GetWeapon(id);
        if (!base)
            return {};
        WeaponDef w = *base;
        if (const FactionDef* fd = GetFaction(f))
        {
            w.rofRpm *= fd->rofMult;
            w.damage *= fd->damageMult;
            w.minDamage *= fd->damageMult;
            w.reloadSec *= fd->reloadMult;
            w.gravity *= fd->projGravityMult;
        }
        return w;
    }

    void TFDataTables::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Data Tables"))
            return;
        ImGui::Text("Loaded: %s", m_loaded ? "yes" : "NO");
        ImGui::Text("Continent: %s (%.0fm, %zu regions)", m_continent.name.c_str(), m_continent.sizeM,
                    m_continent.regions.size());
        ImGui::Text("Weapons: %zu | Classes: %zu | Vehicles: %zu | Factions: %zu", m_weapons.size(), m_classes.size(),
                    m_vehicles.size(), m_factions.size());
        if (ImGui::Button("Reload (tf_reload_data)"))
            ReloadAll();
#endif
    }

} // namespace Terrafront
