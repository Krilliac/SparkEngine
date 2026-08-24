/**
 * @file TFDataTablesParseWorld.cpp
 * @brief World-table parsers (regions/continent lattice, presentation,
 *        deployable visuals) with the same fail-loud validation as before:
 *        contiguous region ids, conduit symmetry, complete initial ownership,
 *        mandatory base deployable kinds. Split from TFDataTables.cpp; the
 *        shared JSON accessors live in TFDataTablesInternal.h, the roster
 *        tables in TFDataTablesParseRoster.cpp.
 */
#include "Data/TFDataTablesInternal.h"
#include "Game/TFDeployableTypes.h" // W6: extended DeployableKind constants

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Terrafront
{
    namespace DataTablesDetail
    {

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

            // Skybox overrides were added at the document root by the
            // per-zone art pass while the original schema kept them under
            // "presentation". Accept both layouts and give an explicit root
            // override precedence; a malformed override must fail loudly
            // instead of silently falling back to stale nested art.
            const Value& sky = root.HasKey("skybox") ? root["skybox"] : pres["skybox"];
            if (!sky.IsObject() || !sky["faceTex"].IsArray() || sky["faceTex"].Size() != 6)
            {
                err = "presentation.json: skybox.faceTex (root or presentation.skybox) must have exactly 6 entries";
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

            // Sanctuary sky: defaults to the continent sky, overridden by an
            // optional "sanctuarySkybox" object (same shape). Faces are only
            // replaced when a full 6-face array is present.
            out.sanctuarySkybox = out.skybox;
            if (root.HasKey("sanctuarySkybox"))
            {
                // The new root schema is a complete asset-set override and is
                // therefore strict. It also deliberately wins over legacy nested data.
                const Value& ssky = root["sanctuarySkybox"];
                if (!ssky.IsObject() || !ssky["faceTex"].IsArray() || ssky["faceTex"].Size() != 6)
                {
                    err = "presentation.json: root sanctuarySkybox.faceTex must have exactly 6 entries";
                    return false;
                }
                for (size_t i = 0; i < 6; ++i)
                    out.sanctuarySkybox.faceTex[i] = ssky["faceTex"][i].AsString(out.sanctuarySkybox.faceTex[i]);
                out.sanctuarySkybox.scale = GetNum(ssky, "scale", out.sanctuarySkybox.scale);
                if (ssky["tint"].IsArray() && ssky["tint"].Size() == 4)
                    for (size_t i = 0; i < 4; ++i)
                        out.sanctuarySkybox.tint[i] =
                            static_cast<float>(ssky["tint"][i].AsNumber(out.sanctuarySkybox.tint[i]));
            }
            else if (pres.HasKey("sanctuarySkybox"))
            {
                // Compatibility: the original nested schema allowed a partial
                // scale/tint-only override and ignored faceTex unless all six
                // faces were supplied.
                const Value& ssky = pres["sanctuarySkybox"];
                if (!ssky.IsObject())
                {
                    err = "presentation.json: presentation.sanctuarySkybox must be an object";
                    return false;
                }
                if (ssky["faceTex"].IsArray() && ssky["faceTex"].Size() == 6)
                    for (size_t i = 0; i < 6; ++i)
                        out.sanctuarySkybox.faceTex[i] = ssky["faceTex"][i].AsString(out.sanctuarySkybox.faceTex[i]);
                out.sanctuarySkybox.scale = GetNum(ssky, "scale", out.sanctuarySkybox.scale);
                if (ssky["tint"].IsArray() && ssky["tint"].Size() == 4)
                    for (size_t i = 0; i < 4; ++i)
                        out.sanctuarySkybox.tint[i] =
                            static_cast<float>(ssky["tint"][i].AsNumber(out.sanctuarySkybox.tint[i]));
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

    } // namespace DataTablesDetail
} // namespace Terrafront
