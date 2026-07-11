/**
 * @file RegionMapEditorPanel.cpp
 * @brief Visual editor for Assets/MMOFPS/Data/regions.json (W11)
 * @author Spark Engine Team
 * @date 2026
 *
 * Schema contract — the writer emits EXACTLY the fields consumed by the game
 * module's TFDataTables::ParseRegions (GameModules/SparkGameMMOFPS/Source/Data/
 * TFDataTables.cpp), in a stable order:
 *
 *   root:      "$schema_note" (preserved verbatim), "continent", "regions",
 *              "conduits", "initialOwnership"
 *   continent: "name", "sizeM", "scene", "fluxTickSec"
 *   region:    "id", "key", "name", "tier", "homeFaction" (only when set),
 *              "hex" [q,r] (only when set), "center" [x,z], "captureSec",
 *              "fluxPerTick", "capturePoints" [[x,z]...], "spawns" [[x,z]...],
 *              "vehicleTerminal" [x,z] or null
 *   conduits:  [[idA,idB]...] (file order preserved)
 *   initialOwnership: "MRA", "AUC", "HLX", "neutral" region-id lists
 *
 * Unknown/extra keys in a hand-edited file are NOT round-tripped — saving
 * normalizes the file to the schema above. Spark::Json::Value stores objects
 * in an unordered_map, so StringifyPretty cannot give a stable field order;
 * the writer below is hand-rolled for that reason.
 *
 * Saving backs up to regions.json.bak first, then re-reads the written file
 * through Json::ParseStrict; on failure the backup is restored. Validation
 * (BFS skyanchor reachability, capture-point radius heuristic, orphan links,
 * plus every hard error ParseRegions raises) blocks the save until the
 * 'I know what I am doing' checkbox is ticked.
 *
 * This panel edits the DATA FILE only — no live world interaction.
 */

#include "RegionMapEditorPanel.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace SparkEditor
{

    namespace
    {
        // Mirrors of the game-module limits (Core/TFTypes.h). The editor cannot
        // include game-module headers, so the values are duplicated here; keep
        // them in sync with kMaxRegions / kMaxCapturePoints.
        constexpr int kMaxRegions = 64;
        constexpr int kMaxCapturePoints = 3;

        constexpr const char* kDataRelPath = "Assets/MMOFPS/Data/regions.json";

        const char* const kTierNames[] = {"skyanchor", "outpost", "fort", "facility"};
        const char* const kFactionTags[] = {"MRA", "AUC", "HLX"};

        bool IsValidTier(const std::string& tier)
        {
            for (const char* t : kTierNames)
                if (tier == t)
                    return true;
            return false;
        }

        bool IsFactionTag(const std::string& tag)
        {
            for (const char* t : kFactionTags)
                if (tag == t)
                    return true;
            return false;
        }

        /// @brief Number formatting for the writer: integers without a decimal
        ///        point (matches the hand-authored file), fractions trimmed.
        std::string FormatNum(double v)
        {
            if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
                return buf;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.3f", v);
            std::string s = buf;
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            if (!s.empty() && s.back() == '.')
                s.pop_back();
            return s;
        }

        void AppendEscaped(std::string& out, const std::string& in)
        {
            out += '"';
            for (char c : in)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
                }
            }
            out += '"';
        }

        void AppendXZList(std::string& out, const std::vector<std::array<float, 2>>& pts)
        {
            out += '[';
            for (size_t i = 0; i < pts.size(); ++i)
            {
                if (i > 0)
                    out += ", ";
                out += '[';
                out += FormatNum(pts[i][0]);
                out += ", ";
                out += FormatNum(pts[i][1]);
                out += ']';
            }
            out += ']';
        }

        // Lenient JSON getters (load must tolerate broken files so the user can
        // FIX them in the editor; RunValidation reports what the game rejects).
        std::string GetStr(const Spark::Json::Value& o, const char* key, const char* fallback = "")
        {
            return (o.HasKey(key) && o[key].IsString()) ? o[key].AsString() : std::string(fallback);
        }

        float GetNum(const Spark::Json::Value& o, const char* key, float fallback)
        {
            return (o.HasKey(key) && o[key].IsNumber()) ? static_cast<float>(o[key].AsNumber()) : fallback;
        }

        int GetInt(const Spark::Json::Value& o, const char* key, int fallback)
        {
            return (o.HasKey(key) && o[key].IsNumber()) ? o[key].AsInt(fallback) : fallback;
        }

        bool ReadXZList(const Spark::Json::Value& o, const char* key, std::vector<std::array<float, 2>>& dst)
        {
            if (!o.HasKey(key) || !o[key].IsArray())
                return true; // absent == empty (same as ParseRegions)
            for (size_t p = 0; p < o[key].Size(); ++p)
            {
                const Spark::Json::Value& pt = o[key][p];
                if (!pt.IsArray() || pt.Size() != 2)
                    return false;
                dst.push_back({static_cast<float>(pt[0].AsNumber(0.0)), static_cast<float>(pt[1].AsNumber(0.0))});
            }
            return true;
        }

        ImU32 TierFillColor(const std::string& tier)
        {
            if (tier == "skyanchor")
                return IM_COL32(255, 215, 90, 110);
            if (tier == "fort")
                return IM_COL32(230, 140, 80, 100);
            if (tier == "facility")
                return IM_COL32(170, 110, 240, 110);
            if (tier == "outpost")
                return IM_COL32(120, 200, 120, 90);
            return IM_COL32(220, 70, 70, 120); // unknown tier: alarming red
        }

        ImU32 OwnerColor(const std::string& owner)
        {
            if (owner == "MRA")
                return IM_COL32(80, 140, 255, 255);
            if (owner == "AUC")
                return IM_COL32(255, 170, 60, 255);
            if (owner == "HLX")
                return IM_COL32(200, 90, 255, 255);
            return IM_COL32(150, 150, 150, 255); // neutral / unassigned
        }

        float TierWorldRadius(const std::string& tier)
        {
            if (tier == "skyanchor")
                return 190.0f;
            if (tier == "facility")
                return 165.0f;
            if (tier == "fort")
                return 140.0f;
            return 110.0f;
        }

        float Dist2(float ax, float ay, float bx, float by)
        {
            const float dx = ax - bx;
            const float dy = ay - by;
            return dx * dx + dy * dy;
        }
    } // namespace

    // ========================================================================
    // Construction / lifecycle
    // ========================================================================

    RegionMapEditorPanel::RegionMapEditorPanel() : EditorPanel("Region Map Editor", "region_map_editor_panel")
    {
        m_category = PanelCategory::Tool;
    }

    bool RegionMapEditorPanel::Initialize()
    {
        ResolveDataPath();
        std::string err;
        if (LoadFromDisk(err))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "RegionMapEditorPanel: loaded %d region(s) from '%s'",
                           static_cast<int>(m_regions.size()), m_dataPath.c_str());
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "RegionMapEditorPanel: failed to load '%s': %s",
                           m_dataPath.c_str(), err.c_str());
        }
        return true; // the panel is still useful (shows the error + Reload button)
    }

    void RegionMapEditorPanel::Update(float /*deltaTime*/) {}

    void RegionMapEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Region Map Editor panel");
    }

    // ========================================================================
    // Load / save
    // ========================================================================

    void RegionMapEditorPanel::ResolveDataPath()
    {
        // The editor normally runs with the repo root as cwd, but probe a few
        // parents so the panel also works when launched from a build output
        // directory (same probe as SceneImportPanel / BasicMaterialEditorPanel).
        m_assetsPrefix.clear();
        for (const char* prefix : {"", "../", "../../", "../../../"})
        {
            std::error_code ec;
            if (fs::exists(fs::path(prefix) / "Assets" / "MMOFPS" / "Data", ec))
            {
                m_assetsPrefix = prefix;
                break;
            }
        }
        m_dataPath = m_assetsPrefix + kDataRelPath;
    }

    bool RegionMapEditorPanel::LoadFromDisk(std::string& outError)
    {
        m_loaded = false;
        m_dirty = false;
        m_regions.clear();
        m_conduits.clear();
        m_schemaNote.clear();
        m_selected = -1;
        m_linkFirst = -1;
        m_bufForRegion = -1;
        m_validationRan = false;
        m_violations.clear();
        m_overrideSave = false;
        m_fitRequested = true;

        std::ifstream file(m_dataPath, std::ios::binary);
        if (!file.is_open())
        {
            outError = "cannot open '" + m_dataPath + "'";
            m_statusMsg = outError;
            m_statusIsError = true;
            return false;
        }
        std::stringstream ss;
        ss << file.rdbuf();
        const std::string text = ss.str();

        Spark::Json::Value root;
        std::string parseErr;
        if (!Spark::Json::ParseStrict(text, &root, &parseErr))
        {
            outError = "JSON parse error: " + parseErr;
            m_statusMsg = outError;
            m_statusIsError = true;
            return false;
        }
        if (!root.IsObject())
        {
            outError = "root is not an object";
            m_statusMsg = outError;
            m_statusIsError = true;
            return false;
        }

        m_schemaNote = GetStr(root, "$schema_note");

        // Continent (lenient, same fallbacks as ParseRegions).
        const Spark::Json::Value& cont = root["continent"];
        m_continent = Continent{};
        if (cont.IsObject())
        {
            m_continent.name = GetStr(cont, "name", "Cindral Wastes");
            m_continent.sizeM = GetNum(cont, "sizeM", 4096.0f);
            m_continent.scene = GetStr(cont, "scene");
            m_continent.fluxTickSec = GetNum(cont, "fluxTickSec", 60.0f);
        }

        // Regions (lenient: a broken entry is loaded as-is so the user can fix it;
        // RunValidation reports everything ParseRegions would reject).
        const Spark::Json::Value& arr = root["regions"];
        if (arr.IsArray())
        {
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const Spark::Json::Value& o = arr[i];
                if (!o.IsObject())
                    continue;
                Region r;
                r.id = GetInt(o, "id", -1);
                r.key = GetStr(o, "key");
                r.name = GetStr(o, "name");
                r.tier = GetStr(o, "tier");
                r.homeFaction = GetStr(o, "homeFaction");
                if (o.HasKey("hex") && o["hex"].IsArray() && o["hex"].Size() == 2)
                {
                    r.hasHex = true;
                    r.hexQ = o["hex"][0].AsInt(0);
                    r.hexR = o["hex"][1].AsInt(0);
                }
                if (o.HasKey("center") && o["center"].IsArray() && o["center"].Size() == 2)
                {
                    r.centerX = static_cast<float>(o["center"][0].AsNumber(0.0));
                    r.centerZ = static_cast<float>(o["center"][1].AsNumber(0.0));
                }
                r.captureSec = GetNum(o, "captureSec", 60.0f);
                r.fluxPerTick = GetInt(o, "fluxPerTick", 0);
                ReadXZList(o, "capturePoints", r.capturePoints);
                ReadXZList(o, "spawns", r.spawns);
                if (o.HasKey("vehicleTerminal") && o["vehicleTerminal"].IsArray() && o["vehicleTerminal"].Size() == 2)
                {
                    r.hasVehicleTerminal = true;
                    r.vehicleTerminal = {static_cast<float>(o["vehicleTerminal"][0].AsNumber(0.0)),
                                         static_cast<float>(o["vehicleTerminal"][1].AsNumber(0.0))};
                }
                m_regions.push_back(std::move(r));
            }
        }
        std::sort(m_regions.begin(), m_regions.end(), [](const Region& a, const Region& b) { return a.id < b.id; });

        // Conduits (file order preserved).
        const Spark::Json::Value& conduits = root["conduits"];
        if (conduits.IsArray())
        {
            for (size_t i = 0; i < conduits.Size(); ++i)
            {
                const Spark::Json::Value& c = conduits[i];
                if (c.IsArray() && c.Size() == 2)
                    m_conduits.emplace_back(c[0].AsInt(-1), c[1].AsInt(-1));
            }
        }

        // Initial ownership -> per-region owner tag ("" = unassigned, flagged later).
        std::unordered_map<int, size_t> idToIndex;
        for (size_t i = 0; i < m_regions.size(); ++i)
            idToIndex[m_regions[i].id] = i;
        const Spark::Json::Value& own = root["initialOwnership"];
        if (own.IsObject())
        {
            const char* const buckets[] = {"MRA", "AUC", "HLX", "neutral"};
            for (const char* bucket : buckets)
            {
                if (!own.HasKey(bucket) || !own[bucket].IsArray())
                    continue;
                for (size_t i = 0; i < own[bucket].Size(); ++i)
                {
                    auto it = idToIndex.find(own[bucket][i].AsInt(-1));
                    if (it != idToIndex.end())
                        m_regions[it->second].owner = bucket;
                }
            }
        }

        // Continent edit buffers.
        std::snprintf(m_continentNameBuf, sizeof(m_continentNameBuf), "%s", m_continent.name.c_str());
        std::snprintf(m_sceneBuf, sizeof(m_sceneBuf), "%s", m_continent.scene.c_str());

        m_loaded = true;
        m_statusMsg = "Loaded " + std::to_string(m_regions.size()) + " regions, " + std::to_string(m_conduits.size()) +
                      " conduits.";
        m_statusIsError = false;
        return true;
    }

    std::string RegionMapEditorPanel::SerializeDocument() const
    {
        std::string out;
        out.reserve(8192);
        out += "{\n";

        if (!m_schemaNote.empty())
        {
            out += "  \"$schema_note\": ";
            AppendEscaped(out, m_schemaNote);
            out += ",\n";
        }

        out += "  \"continent\": {\n";
        out += "    \"name\": ";
        AppendEscaped(out, m_continent.name);
        out += ",\n    \"sizeM\": " + FormatNum(m_continent.sizeM);
        out += ",\n    \"scene\": ";
        AppendEscaped(out, m_continent.scene);
        out += ",\n    \"fluxTickSec\": " + FormatNum(m_continent.fluxTickSec);
        out += "\n  },\n";

        out += "  \"regions\": [\n";
        for (size_t i = 0; i < m_regions.size(); ++i)
        {
            const Region& r = m_regions[i];
            out += "    {\n";
            out += "      \"id\": " + std::to_string(r.id) + ",\n";
            out += "      \"key\": ";
            AppendEscaped(out, r.key);
            out += ",\n      \"name\": ";
            AppendEscaped(out, r.name);
            out += ",\n      \"tier\": ";
            AppendEscaped(out, r.tier);
            if (!r.homeFaction.empty())
            {
                out += ",\n      \"homeFaction\": ";
                AppendEscaped(out, r.homeFaction);
            }
            if (r.hasHex)
            {
                out += ",\n      \"hex\": [" + std::to_string(r.hexQ) + ", " + std::to_string(r.hexR) + "]";
            }
            out += ",\n      \"center\": [" + FormatNum(r.centerX) + ", " + FormatNum(r.centerZ) + "]";
            out += ",\n      \"captureSec\": " + FormatNum(r.captureSec);
            out += ",\n      \"fluxPerTick\": " + std::to_string(r.fluxPerTick);
            out += ",\n      \"capturePoints\": ";
            AppendXZList(out, r.capturePoints);
            out += ",\n      \"spawns\": ";
            AppendXZList(out, r.spawns);
            out += ",\n      \"vehicleTerminal\": ";
            if (r.hasVehicleTerminal)
            {
                out += "[" + FormatNum(r.vehicleTerminal[0]) + ", " + FormatNum(r.vehicleTerminal[1]) + "]";
            }
            else
            {
                out += "null";
            }
            out += "\n    }";
            out += (i + 1 < m_regions.size()) ? ",\n" : "\n";
        }
        out += "  ],\n";

        out += "  \"conduits\": [\n";
        for (size_t i = 0; i < m_conduits.size(); ++i)
        {
            out += "    [" + std::to_string(m_conduits[i].first) + ", " + std::to_string(m_conduits[i].second) + "]";
            out += (i + 1 < m_conduits.size()) ? ",\n" : "\n";
        }
        out += "  ],\n";

        out += "  \"initialOwnership\": {\n";
        const char* const buckets[] = {"MRA", "AUC", "HLX", "neutral"};
        for (size_t b = 0; b < 4; ++b)
        {
            out += "    \"";
            out += buckets[b];
            out += "\": [";
            bool first = true;
            for (const Region& r : m_regions)
            {
                // Unassigned regions land in "neutral" so the emitted file always
                // has the exactly-once ownership ParseRegions requires; validation
                // flags them before the save so this is never silent.
                const std::string& owner = r.owner.empty() ? std::string("neutral") : r.owner;
                if (owner != buckets[b])
                    continue;
                if (!first)
                    out += ", ";
                out += std::to_string(r.id);
                first = false;
            }
            out += (b + 1 < 4) ? "],\n" : "]\n";
        }
        out += "  }\n";

        out += "}\n";
        return out;
    }

    bool RegionMapEditorPanel::SaveToDisk(std::string& outError)
    {
        // 1) Backup the current file (if any) to regions.json.bak.
        std::string oldBytes;
        bool hadOld = false;
        {
            std::ifstream in(m_dataPath, std::ios::binary);
            if (in.is_open())
            {
                std::stringstream ss;
                ss << in.rdbuf();
                oldBytes = ss.str();
                hadOld = true;
            }
        }
        const std::string bakPath = m_dataPath + ".bak";
        if (hadOld)
        {
            std::ofstream bak(bakPath, std::ios::binary | std::ios::trunc);
            if (!bak.is_open())
            {
                outError = "cannot write backup '" + bakPath + "' - save aborted";
                return false;
            }
            bak.write(oldBytes.data(), static_cast<std::streamsize>(oldBytes.size()));
            if (!bak.good())
            {
                outError = "backup write failed for '" + bakPath + "' - save aborted";
                return false;
            }
        }

        // 2) Write the document.
        const std::string doc = SerializeDocument();
        {
            std::ofstream outFile(m_dataPath, std::ios::binary | std::ios::trunc);
            if (!outFile.is_open())
            {
                outError = "cannot open '" + m_dataPath + "' for writing";
                return false;
            }
            outFile.write(doc.data(), static_cast<std::streamsize>(doc.size()));
            if (!outFile.good())
            {
                outError = "write failed for '" + m_dataPath + "'";
                return false;
            }
        }

        // 3) Re-read what actually hit the disk and ParseStrict-validate it.
        {
            std::ifstream verify(m_dataPath, std::ios::binary);
            std::stringstream ss;
            ss << verify.rdbuf();
            const std::string written = ss.str();
            Spark::Json::Value root;
            std::string parseErr;
            if (!Spark::Json::ParseStrict(written, &root, &parseErr))
            {
                // Restore the backup so the game never sees a corrupt file.
                if (hadOld)
                {
                    std::ofstream restore(m_dataPath, std::ios::binary | std::ios::trunc);
                    restore.write(oldBytes.data(), static_cast<std::streamsize>(oldBytes.size()));
                }
                outError = "post-save ParseStrict failed (" + parseErr + ")" +
                           (hadOld ? " - original file restored from backup" : "");
                return false;
            }
        }

        m_dirty = false;
        return true;
    }

    // ========================================================================
    // Validation
    // ========================================================================

    float RegionMapEditorPanel::RegionRadiusHeuristic(size_t regionIndex) const
    {
        // Heuristic footprint: half the distance to the nearest other region
        // center, clamped to a sane minimum so tightly-packed lattices don't
        // produce zero-size regions.
        float best = m_continent.sizeM * m_continent.sizeM;
        const Region& r = m_regions[regionIndex];
        for (size_t i = 0; i < m_regions.size(); ++i)
        {
            if (i == regionIndex)
                continue;
            best = std::min(best, Dist2(r.centerX, r.centerZ, m_regions[i].centerX, m_regions[i].centerZ));
        }
        const float radius = 0.5f * std::sqrt(best);
        return std::max(radius, 150.0f);
    }

    void RegionMapEditorPanel::RunValidation()
    {
        m_violations.clear();
        m_validationRan = true;
        if (!m_loaded)
        {
            m_violations.push_back("no document loaded");
            return;
        }

        const size_t count = m_regions.size();
        if (count == 0)
            m_violations.push_back("regions array is empty (ParseRegions rejects this)");
        if (count > static_cast<size_t>(kMaxRegions))
            m_violations.push_back("too many regions (" + std::to_string(count) + " > " + std::to_string(kMaxRegions) +
                                   ")");

        // Ids contiguous [0..N) — ParseRegions hard-fails otherwise.
        for (size_t i = 0; i < count; ++i)
        {
            if (m_regions[i].id != static_cast<int>(i))
            {
                m_violations.push_back("region ids must be contiguous starting at 0 (index " + std::to_string(i) +
                                       " has id " + std::to_string(m_regions[i].id) + ")");
                break;
            }
        }

        std::unordered_set<std::string> seenKeys;
        for (size_t i = 0; i < count; ++i)
        {
            const Region& r = m_regions[i];
            const std::string tag = "'" + (r.key.empty() ? ("#" + std::to_string(r.id)) : r.key) + "'";
            if (r.key.empty())
                m_violations.push_back(tag + ": empty key");
            else if (!seenKeys.insert(r.key).second)
                m_violations.push_back(tag + ": duplicate key");
            if (!IsValidTier(r.tier))
                m_violations.push_back(tag + ": unknown tier '" + r.tier + "'");
            if (r.tier == "skyanchor" && !IsFactionTag(r.homeFaction))
                m_violations.push_back(tag + ": skyanchor needs a homeFaction (MRA/AUC/HLX)");
            if (!r.homeFaction.empty() && !IsFactionTag(r.homeFaction))
                m_violations.push_back(tag + ": bad homeFaction '" + r.homeFaction + "'");
            if (r.spawns.empty())
                m_violations.push_back(tag + ": no spawns (every region needs at least one)");
            if (r.tier != "skyanchor" && r.capturePoints.empty())
                m_violations.push_back(tag + ": capturable region has no capturePoints");
            if (r.capturePoints.size() > static_cast<size_t>(kMaxCapturePoints))
                m_violations.push_back(tag + ": exceeds kMaxCapturePoints (" + std::to_string(kMaxCapturePoints) + ")");
            if (r.owner.empty())
                m_violations.push_back(tag + ": not in initialOwnership (will be written as neutral)");
            if (r.centerX < 0.0f || r.centerX > m_continent.sizeM || r.centerZ < 0.0f || r.centerZ > m_continent.sizeM)
                m_violations.push_back(tag + ": center outside world bounds [0.." + FormatNum(m_continent.sizeM) + "]");

            // Radius heuristic: capture points / spawns / terminal should sit
            // within the region's estimated footprint.
            const float radius = count > 1 ? RegionRadiusHeuristic(i) : m_continent.sizeM;
            const float r2 = radius * radius;
            auto checkPts = [&](const std::vector<std::array<float, 2>>& pts, const char* what)
            {
                for (size_t p = 0; p < pts.size(); ++p)
                {
                    if (Dist2(pts[p][0], pts[p][1], r.centerX, r.centerZ) > r2)
                        m_violations.push_back(tag + ": " + what + " " + std::to_string(p) +
                                               " lies outside the region radius heuristic (~" + FormatNum(radius) +
                                               " m)");
                }
            };
            checkPts(r.capturePoints, "capture point");
            checkPts(r.spawns, "spawn");
            if (r.hasVehicleTerminal && Dist2(r.vehicleTerminal[0], r.vehicleTerminal[1], r.centerX, r.centerZ) > r2)
                m_violations.push_back(tag + ": vehicle terminal lies outside the region radius heuristic (~" +
                                       FormatNum(radius) + " m)");
        }

        // Conduits: no orphan links, no self-links, no duplicates; non-empty.
        if (m_conduits.empty())
            m_violations.push_back("conduits array is empty (ParseRegions rejects this)");
        std::unordered_set<uint64_t> seenLinks;
        std::vector<std::vector<int>> adj(count);
        for (size_t i = 0; i < m_conduits.size(); ++i)
        {
            const int a = m_conduits[i].first;
            const int b = m_conduits[i].second;
            const std::string ctag = "conduit [" + std::to_string(a) + "," + std::to_string(b) + "]";
            if (a < 0 || b < 0 || a >= static_cast<int>(count) || b >= static_cast<int>(count))
            {
                m_violations.push_back(ctag + ": orphan link (endpoint is not a region id)");
                continue;
            }
            if (a == b)
            {
                m_violations.push_back(ctag + ": self-link");
                continue;
            }
            const uint64_t lo = static_cast<uint64_t>(std::min(a, b));
            const uint64_t hi = static_cast<uint64_t>(std::max(a, b));
            if (!seenLinks.insert((hi << 32) | lo).second)
                m_violations.push_back(ctag + ": duplicate link");
            adj[static_cast<size_t>(a)].push_back(b);
            adj[static_cast<size_t>(b)].push_back(a);
        }

        // BFS: every non-skyanchor region reachable from at least one skyanchor.
        if (count > 0)
        {
            std::vector<bool> reached(count, false);
            std::vector<int> queue;
            for (size_t i = 0; i < count; ++i)
            {
                if (m_regions[i].tier == "skyanchor")
                {
                    reached[i] = true;
                    queue.push_back(static_cast<int>(i));
                }
            }
            if (queue.empty())
                m_violations.push_back("no skyanchor regions - nothing is reachable");
            for (size_t head = 0; head < queue.size(); ++head)
            {
                for (int n : adj[static_cast<size_t>(queue[head])])
                {
                    if (n >= 0 && n < static_cast<int>(count) && !reached[static_cast<size_t>(n)])
                    {
                        reached[static_cast<size_t>(n)] = true;
                        queue.push_back(n);
                    }
                }
            }
            for (size_t i = 0; i < count; ++i)
            {
                if (!reached[i] && m_regions[i].tier != "skyanchor")
                    m_violations.push_back("'" + m_regions[i].key + "': not reachable from any skyanchor via conduits");
            }
        }
    }

    // ========================================================================
    // Editing helpers
    // ========================================================================

    void RegionMapEditorPanel::ToggleConduit(int a, int b)
    {
        if (a == b || a < 0 || b < 0)
            return;
        for (size_t i = 0; i < m_conduits.size(); ++i)
        {
            const auto& c = m_conduits[i];
            if ((c.first == a && c.second == b) || (c.first == b && c.second == a))
            {
                m_conduits.erase(m_conduits.begin() + static_cast<std::ptrdiff_t>(i));
                m_dirty = true;
                m_validationRan = false;
                return;
            }
        }
        m_conduits.emplace_back(a, b);
        m_dirty = true;
        m_validationRan = false;
    }

    void RegionMapEditorPanel::SelectRegion(int index)
    {
        m_selected = index;
        SyncEditBuffers();
    }

    void RegionMapEditorPanel::SyncEditBuffers()
    {
        if (m_selected == m_bufForRegion)
            return;
        m_bufForRegion = m_selected;
        if (m_selected >= 0 && m_selected < static_cast<int>(m_regions.size()))
        {
            const Region& r = m_regions[static_cast<size_t>(m_selected)];
            std::snprintf(m_keyBuf, sizeof(m_keyBuf), "%s", r.key.c_str());
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", r.name.c_str());
        }
    }

    // ========================================================================
    // Rendering
    // ========================================================================

    void RegionMapEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float sideW = 360.0f;
            const float canvasW = std::max(140.0f, avail.x - sideW - 8.0f);

            ImGui::BeginChild("region_canvas_child", ImVec2(canvasW, 0.0f), true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            RenderCanvas();
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("region_side_pane", ImVec2(0.0f, 0.0f), true);
            RenderSidePane();
            ImGui::EndChild();
        }
        EndPanel();
    }

    void RegionMapEditorPanel::RenderToolbar()
    {
        if (ImGui::Button("Reload"))
        {
            std::string err;
            LoadFromDisk(err);
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit View"))
        {
            m_fitRequested = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Link mode", &m_linkMode))
        {
            m_linkFirst = -1;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click region A then region B to toggle a conduit between them.");
        ImGui::SameLine();
        ImGui::Checkbox("Move points with region", &m_movePointsWithRegion);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Dragging a region center also drags its capture points, spawns and terminal.");
        ImGui::SameLine();
        ImGui::TextDisabled("| %s%s", m_dataPath.c_str(), m_dirty ? " *" : "");
    }

    void RegionMapEditorPanel::RenderCanvas()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        size.x = std::max(size.x, 64.0f);
        size.y = std::max(size.y, 64.0f);
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);

        ImGui::InvisibleButton("region_map_canvas", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImGuiIO& io = ImGui::GetIO();

        const float sizeM = std::max(1.0f, m_continent.sizeM);
        const float baseScale = std::min(size.x, size.y) / sizeM;
        if (m_fitRequested)
        {
            m_zoom = 1.0f;
            m_panX = (size.x - sizeM * baseScale) * 0.5f;
            m_panY = (size.y - sizeM * baseScale) * 0.5f;
            m_fitRequested = false;
        }

        // Zoom around the mouse cursor.
        if (hovered && io.MouseWheel != 0.0f)
        {
            const float sOld = baseScale * m_zoom;
            const float newZoom = std::clamp(m_zoom * std::pow(1.15f, io.MouseWheel), 0.2f, 25.0f);
            const float sNew = baseScale * newZoom;
            const float mx = io.MousePos.x - p0.x;
            const float my = io.MousePos.y - p0.y;
            m_panX = mx - (mx - m_panX) * (sNew / sOld);
            m_panY = my - (my - m_panY) * (sNew / sOld);
            m_zoom = newZoom;
        }
        const float s = baseScale * m_zoom;

        auto toScreen = [&](float wx, float wz)
        { return ImVec2(p0.x + m_panX + wx * s, p0.y + m_panY + (sizeM - wz) * s); };

        // Pan with middle or right drag.
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                       ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)))
        {
            m_panX += io.MouseDelta.x;
            m_panY += io.MouseDelta.y;
        }

        // ---- Left-mouse interaction: hit test on press, drag while held ----
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_dragKind = DragKind::None;
            m_dragRegion = -1;
            m_dragPoint = -1;
            const float hitR2 = 8.0f * 8.0f;
            int hitRegion = -1;

            // Markers first (they are small and sit on top of the hexes).
            for (size_t i = 0; i < m_regions.size() && m_dragKind == DragKind::None; ++i)
            {
                const Region& r = m_regions[i];
                for (size_t j = 0; j < r.capturePoints.size(); ++j)
                {
                    const ImVec2 sp = toScreen(r.capturePoints[j][0], r.capturePoints[j][1]);
                    if (Dist2(sp.x, sp.y, io.MousePos.x, io.MousePos.y) < hitR2)
                    {
                        m_dragKind = DragKind::CapturePoint;
                        m_dragRegion = static_cast<int>(i);
                        m_dragPoint = static_cast<int>(j);
                        break;
                    }
                }
                for (size_t j = 0; m_dragKind == DragKind::None && j < r.spawns.size(); ++j)
                {
                    const ImVec2 sp = toScreen(r.spawns[j][0], r.spawns[j][1]);
                    if (Dist2(sp.x, sp.y, io.MousePos.x, io.MousePos.y) < hitR2)
                    {
                        m_dragKind = DragKind::Spawn;
                        m_dragRegion = static_cast<int>(i);
                        m_dragPoint = static_cast<int>(j);
                    }
                }
                if (m_dragKind == DragKind::None && r.hasVehicleTerminal)
                {
                    const ImVec2 sp = toScreen(r.vehicleTerminal[0], r.vehicleTerminal[1]);
                    if (Dist2(sp.x, sp.y, io.MousePos.x, io.MousePos.y) < hitR2)
                    {
                        m_dragKind = DragKind::Terminal;
                        m_dragRegion = static_cast<int>(i);
                    }
                }
            }
            // Region hexes second.
            if (m_dragKind == DragKind::None)
            {
                for (size_t i = 0; i < m_regions.size(); ++i)
                {
                    const Region& r = m_regions[i];
                    const ImVec2 c = toScreen(r.centerX, r.centerZ);
                    const float rpx = std::max(6.0f, TierWorldRadius(r.tier) * s);
                    if (Dist2(c.x, c.y, io.MousePos.x, io.MousePos.y) < rpx * rpx)
                    {
                        hitRegion = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (m_linkMode)
            {
                // Link mode: pure click semantics, no dragging.
                m_dragKind = DragKind::None;
                if (m_dragRegion >= 0)
                    hitRegion = m_dragRegion; // clicking a marker counts as its region
                m_dragRegion = -1;
                m_dragPoint = -1;
                if (hitRegion >= 0)
                {
                    if (m_linkFirst < 0)
                    {
                        m_linkFirst = hitRegion;
                    }
                    else if (m_linkFirst != hitRegion)
                    {
                        ToggleConduit(m_regions[static_cast<size_t>(m_linkFirst)].id,
                                      m_regions[static_cast<size_t>(hitRegion)].id);
                        m_linkFirst = -1;
                    }
                    SelectRegion(hitRegion);
                }
                else
                {
                    m_linkFirst = -1;
                    SelectRegion(-1);
                }
            }
            else
            {
                if (m_dragKind != DragKind::None)
                {
                    SelectRegion(m_dragRegion);
                }
                else if (hitRegion >= 0)
                {
                    m_dragKind = DragKind::RegionCenter;
                    m_dragRegion = hitRegion;
                    SelectRegion(hitRegion);
                }
                else
                {
                    SelectRegion(-1);
                }
            }
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_dragKind = DragKind::None;
        }
        else if (active && m_dragKind != DragKind::None && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) &&
                 (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && m_dragRegion >= 0 &&
                 m_dragRegion < static_cast<int>(m_regions.size()))
        {
            const float dwx = io.MouseDelta.x / s;
            const float dwz = -io.MouseDelta.y / s;
            Region& r = m_regions[static_cast<size_t>(m_dragRegion)];
            auto movePt = [&](std::array<float, 2>& pt)
            {
                pt[0] = std::clamp(pt[0] + dwx, 0.0f, sizeM);
                pt[1] = std::clamp(pt[1] + dwz, 0.0f, sizeM);
            };
            switch (m_dragKind)
            {
            case DragKind::RegionCenter:
                r.centerX = std::clamp(r.centerX + dwx, 0.0f, sizeM);
                r.centerZ = std::clamp(r.centerZ + dwz, 0.0f, sizeM);
                if (m_movePointsWithRegion)
                {
                    for (auto& pt : r.capturePoints)
                        movePt(pt);
                    for (auto& pt : r.spawns)
                        movePt(pt);
                    if (r.hasVehicleTerminal)
                        movePt(r.vehicleTerminal);
                }
                break;
            case DragKind::CapturePoint:
                if (m_dragPoint >= 0 && m_dragPoint < static_cast<int>(r.capturePoints.size()))
                    movePt(r.capturePoints[static_cast<size_t>(m_dragPoint)]);
                break;
            case DragKind::Spawn:
                if (m_dragPoint >= 0 && m_dragPoint < static_cast<int>(r.spawns.size()))
                    movePt(r.spawns[static_cast<size_t>(m_dragPoint)]);
                break;
            case DragKind::Terminal:
                if (r.hasVehicleTerminal)
                    movePt(r.vehicleTerminal);
                break;
            case DragKind::None:
                break;
            }
            m_dirty = true;
            m_validationRan = false;
        }

        // ---- Drawing ----
        dl->PushClipRect(p0, p1, true);
        dl->AddRectFilled(p0, p1, IM_COL32(24, 26, 30, 255));

        // World bounds + coarse grid.
        const ImVec2 wMin = toScreen(0.0f, sizeM);
        const ImVec2 wMax = toScreen(sizeM, 0.0f);
        dl->AddRect(wMin, wMax, IM_COL32(90, 95, 105, 200));
        const float gridStep = 512.0f;
        for (float g = gridStep; g < sizeM; g += gridStep)
        {
            dl->AddLine(toScreen(g, 0.0f), toScreen(g, sizeM), IM_COL32(60, 63, 70, 120));
            dl->AddLine(toScreen(0.0f, g), toScreen(sizeM, g), IM_COL32(60, 63, 70, 120));
        }

        // Conduits underneath the regions.
        for (const auto& c : m_conduits)
        {
            if (c.first < 0 || c.second < 0 || c.first >= static_cast<int>(m_regions.size()) ||
                c.second >= static_cast<int>(m_regions.size()))
                continue;
            const Region& a = m_regions[static_cast<size_t>(c.first)];
            const Region& b = m_regions[static_cast<size_t>(c.second)];
            dl->AddLine(toScreen(a.centerX, a.centerZ), toScreen(b.centerX, b.centerZ), IM_COL32(120, 180, 220, 160),
                        2.0f);
        }
        // Link-mode rubber band.
        if (m_linkMode && m_linkFirst >= 0 && m_linkFirst < static_cast<int>(m_regions.size()))
        {
            const Region& a = m_regions[static_cast<size_t>(m_linkFirst)];
            dl->AddLine(toScreen(a.centerX, a.centerZ), io.MousePos, IM_COL32(255, 255, 120, 200), 1.5f);
        }

        // Regions as pointy-top hexes.
        for (size_t i = 0; i < m_regions.size(); ++i)
        {
            const Region& r = m_regions[i];
            const ImVec2 c = toScreen(r.centerX, r.centerZ);
            const float rpx = std::max(6.0f, TierWorldRadius(r.tier) * s);
            ImVec2 pts[6];
            for (int k = 0; k < 6; ++k)
            {
                const float ang = (static_cast<float>(k) * 60.0f - 30.0f) * 3.14159265f / 180.0f;
                pts[k] = ImVec2(c.x + rpx * std::cos(ang), c.y + rpx * std::sin(ang));
            }
            dl->AddConvexPolyFilled(pts, 6, TierFillColor(r.tier));
            const bool isSel = (static_cast<int>(i) == m_selected);
            const bool isLinkFirst = (m_linkMode && static_cast<int>(i) == m_linkFirst);
            ImU32 outline = OwnerColor(r.owner);
            float thick = 2.0f;
            if (isLinkFirst)
            {
                outline = IM_COL32(255, 255, 120, 255);
                thick = 3.0f;
            }
            else if (isSel)
            {
                outline = IM_COL32(255, 255, 255, 255);
                thick = 3.0f;
            }
            dl->AddPolyline(pts, 6, outline, thick, ImDrawFlags_Closed);

            const std::string& label = r.name.empty() ? r.key : r.name;
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + rpx + 2.0f), IM_COL32(225, 228, 235, 255), label.c_str());

            // Markers: capture points (yellow circles), spawns (green dots),
            // vehicle terminal (cyan square).
            for (const auto& cp : r.capturePoints)
            {
                const ImVec2 sp = toScreen(cp[0], cp[1]);
                dl->AddCircleFilled(sp, 5.0f, IM_COL32(250, 210, 70, 255));
                dl->AddCircle(sp, 5.0f, IM_COL32(30, 30, 30, 255));
            }
            for (const auto& sw : r.spawns)
            {
                const ImVec2 sp = toScreen(sw[0], sw[1]);
                dl->AddCircleFilled(sp, 3.5f, IM_COL32(110, 230, 110, 255));
            }
            if (r.hasVehicleTerminal)
            {
                const ImVec2 sp = toScreen(r.vehicleTerminal[0], r.vehicleTerminal[1]);
                dl->AddRectFilled(ImVec2(sp.x - 4.5f, sp.y - 4.5f), ImVec2(sp.x + 4.5f, sp.y + 4.5f),
                                  IM_COL32(90, 220, 235, 255));
                dl->AddRect(ImVec2(sp.x - 4.5f, sp.y - 4.5f), ImVec2(sp.x + 4.5f, sp.y + 4.5f),
                            IM_COL32(30, 30, 30, 255));
            }
        }

        // Legend / hint line.
        const char* hint = m_linkMode ? (m_linkFirst >= 0 ? "Link mode: click the second region (click empty to cancel)"
                                                          : "Link mode: click the first region")
                                      : "Drag hexes/markers to move. Wheel = zoom, right/middle drag = pan.";
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 4.0f), IM_COL32(170, 175, 185, 255), hint);
        dl->PopClipRect();
    }

    void RegionMapEditorPanel::RenderSidePane()
    {
        if (!m_statusMsg.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  m_statusIsError ? IM_COL32(255, 110, 110, 255) : IM_COL32(150, 210, 150, 255));
            ImGui::TextWrapped("%s", m_statusMsg.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
        if (!m_loaded)
        {
            ImGui::TextWrapped("Could not load '%s'. Fix the file (or its location) and press Reload.",
                               m_dataPath.c_str());
            return;
        }

        if (m_selected >= 0 && m_selected < static_cast<int>(m_regions.size()))
            RenderRegionEditor();
        else
            RenderContinentEditor();

        ImGui::Separator();
        RenderValidationAndSave();
    }

    void RegionMapEditorPanel::RenderContinentEditor()
    {
        ImGui::TextUnformatted("Continent");
        ImGui::Separator();
        auto markDirty = [this]
        {
            m_dirty = true;
            m_validationRan = false;
        };
        if (ImGui::InputText("Name", m_continentNameBuf, sizeof(m_continentNameBuf)))
        {
            m_continent.name = m_continentNameBuf;
            markDirty();
        }
        if (ImGui::InputText("Scene", m_sceneBuf, sizeof(m_sceneBuf)))
        {
            m_continent.scene = m_sceneBuf;
            markDirty();
        }
        if (ImGui::DragFloat("Size (m)", &m_continent.sizeM, 32.0f, 256.0f, 65536.0f, "%.0f"))
            markDirty();
        if (ImGui::DragFloat("Flux tick (s)", &m_continent.fluxTickSec, 1.0f, 1.0f, 3600.0f, "%.0f"))
            markDirty();
        ImGui::Spacing();
        ImGui::TextWrapped("Select a region on the map to edit it. Enable Link mode to toggle conduits.");
    }

    void RegionMapEditorPanel::RenderRegionEditor()
    {
        Region& r = m_regions[static_cast<size_t>(m_selected)];
        SyncEditBuffers();
        auto markDirty = [this]
        {
            m_dirty = true;
            m_validationRan = false;
        };

        ImGui::Text("Region #%d", r.id);
        ImGui::Separator();

        if (ImGui::InputText("Key", m_keyBuf, sizeof(m_keyBuf)))
        {
            r.key = m_keyBuf;
            markDirty();
        }
        if (ImGui::InputText("Name", m_nameBuf, sizeof(m_nameBuf)))
        {
            r.name = m_nameBuf;
            markDirty();
        }

        // Tier dropdown.
        int tierIdx = -1;
        for (int i = 0; i < 4; ++i)
            if (r.tier == kTierNames[i])
                tierIdx = i;
        const char* tierPreview = tierIdx >= 0 ? kTierNames[tierIdx] : r.tier.c_str();
        if (ImGui::BeginCombo("Tier", tierPreview))
        {
            for (int i = 0; i < 4; ++i)
            {
                if (ImGui::Selectable(kTierNames[i], i == tierIdx))
                {
                    r.tier = kTierNames[i];
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }

        // Home faction (skyanchors require one; others usually omit it).
        const char* facPreview = r.homeFaction.empty() ? "(none)" : r.homeFaction.c_str();
        if (ImGui::BeginCombo("Home faction", facPreview))
        {
            if (ImGui::Selectable("(none)", r.homeFaction.empty()))
            {
                r.homeFaction.clear();
                markDirty();
            }
            for (const char* f : kFactionTags)
            {
                if (ImGui::Selectable(f, r.homeFaction == f))
                {
                    r.homeFaction = f;
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }

        // Initial owner bucket.
        const char* ownPreview = r.owner.empty() ? "(unassigned)" : r.owner.c_str();
        if (ImGui::BeginCombo("Initial owner", ownPreview))
        {
            const char* const owners[] = {"neutral", "MRA", "AUC", "HLX"};
            for (const char* o : owners)
            {
                if (ImGui::Selectable(o, r.owner == o))
                {
                    r.owner = o;
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }

        float center[2] = {r.centerX, r.centerZ};
        if (ImGui::DragFloat2("Center X/Z", center, 4.0f, 0.0f, m_continent.sizeM, "%.0f"))
        {
            r.centerX = center[0];
            r.centerZ = center[1];
            markDirty();
        }
        if (ImGui::Checkbox("Has hex coords", &r.hasHex))
            markDirty();
        if (r.hasHex)
        {
            int hex[2] = {r.hexQ, r.hexR};
            if (ImGui::DragInt2("Hex q/r", hex, 0.1f, -16, 16))
            {
                r.hexQ = hex[0];
                r.hexR = hex[1];
                markDirty();
            }
        }
        if (ImGui::DragFloat("Capture (s)", &r.captureSec, 1.0f, 0.0f, 3600.0f, "%.0f"))
            markDirty();
        if (ImGui::DragInt("Flux per tick", &r.fluxPerTick, 0.1f, 0, 100))
            markDirty();

        // Capture points.
        ImGui::Spacing();
        ImGui::Text("Capture points (%d/%d)", static_cast<int>(r.capturePoints.size()), kMaxCapturePoints);
        for (size_t j = 0; j < r.capturePoints.size(); ++j)
        {
            ImGui::PushID(static_cast<int>(j));
            if (ImGui::DragFloat2("##cp", r.capturePoints[j].data(), 2.0f, 0.0f, m_continent.sizeM, "%.0f"))
                markDirty();
            ImGui::SameLine();
            if (ImGui::SmallButton("X##cp"))
            {
                r.capturePoints.erase(r.capturePoints.begin() + static_cast<std::ptrdiff_t>(j));
                markDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (static_cast<int>(r.capturePoints.size()) < kMaxCapturePoints && ImGui::SmallButton("Add capture point"))
        {
            r.capturePoints.push_back({r.centerX + 40.0f, r.centerZ});
            markDirty();
        }

        // Spawns.
        ImGui::Spacing();
        ImGui::Text("Spawns (%d)", static_cast<int>(r.spawns.size()));
        for (size_t j = 0; j < r.spawns.size(); ++j)
        {
            ImGui::PushID(1000 + static_cast<int>(j));
            if (ImGui::DragFloat2("##sp", r.spawns[j].data(), 2.0f, 0.0f, m_continent.sizeM, "%.0f"))
                markDirty();
            ImGui::SameLine();
            if (ImGui::SmallButton("X##sp"))
            {
                r.spawns.erase(r.spawns.begin() + static_cast<std::ptrdiff_t>(j));
                markDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Add spawn"))
        {
            r.spawns.push_back({r.centerX, r.centerZ - 40.0f});
            markDirty();
        }

        // Vehicle terminal.
        ImGui::Spacing();
        if (ImGui::Checkbox("Vehicle terminal", &r.hasVehicleTerminal))
        {
            if (r.hasVehicleTerminal && r.vehicleTerminal[0] == 0.0f && r.vehicleTerminal[1] == 0.0f)
                r.vehicleTerminal = {r.centerX, r.centerZ + 60.0f};
            markDirty();
        }
        if (r.hasVehicleTerminal)
        {
            if (ImGui::DragFloat2("Terminal X/Z", r.vehicleTerminal.data(), 2.0f, 0.0f, m_continent.sizeM, "%.0f"))
                markDirty();
        }

        // Conduits of this region.
        ImGui::Spacing();
        ImGui::TextUnformatted("Conduits");
        for (size_t i = 0; i < m_conduits.size(); ++i)
        {
            const auto& c = m_conduits[i];
            if (c.first != r.id && c.second != r.id)
                continue;
            const int otherId = (c.first == r.id) ? c.second : c.first;
            const char* otherKey = (otherId >= 0 && otherId < static_cast<int>(m_regions.size()))
                                       ? m_regions[static_cast<size_t>(otherId)].key.c_str()
                                       : "<orphan>";
            ImGui::PushID(2000 + static_cast<int>(i));
            ImGui::BulletText("-> %d (%s)", otherId, otherKey);
            ImGui::SameLine();
            if (ImGui::SmallButton("X##cd"))
            {
                m_conduits.erase(m_conduits.begin() + static_cast<std::ptrdiff_t>(i));
                markDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    void RegionMapEditorPanel::RenderValidationAndSave()
    {
        if (ImGui::Button("Validate"))
            RunValidation();
        ImGui::SameLine();
        const bool blocked = !m_validationRan || !m_violations.empty();
        if (ImGui::Button("Save regions.json"))
        {
            RunValidation();
            if (m_violations.empty() || m_overrideSave)
            {
                std::string err;
                if (SaveToDisk(err))
                {
                    m_statusMsg = "Saved. Backup written to regions.json.bak; ParseStrict re-read OK.";
                    m_statusIsError = false;
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "RegionMapEditorPanel: saved '%s'", m_dataPath.c_str());
                }
                else
                {
                    m_statusMsg = "Save FAILED: " + err;
                    m_statusIsError = true;
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor, "RegionMapEditorPanel: %s", m_statusMsg.c_str());
                }
            }
            else
            {
                m_statusMsg = "Save blocked: " + std::to_string(m_violations.size()) +
                              " violation(s). Fix them or tick the override checkbox.";
                m_statusIsError = true;
            }
        }
        if (blocked && ImGui::IsItemHovered())
            ImGui::SetTooltip("Validation runs automatically on save; violations block it unless overridden.");

        if (m_validationRan)
        {
            if (m_violations.empty())
            {
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                                   "Validation OK - the game will accept this file.");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                                   "%d violation(s):", static_cast<int>(m_violations.size()));
                ImGui::BeginChild("violation_list", ImVec2(0.0f, 140.0f), true);
                for (const std::string& v : m_violations)
                    ImGui::TextWrapped("- %s", v.c_str());
                ImGui::EndChild();
                ImGui::Checkbox("I know what I am doing (save despite violations)", &m_overrideSave);
            }
        }
    }

} // namespace SparkEditor
