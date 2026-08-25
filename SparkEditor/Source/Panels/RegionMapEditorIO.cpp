/**
 * @file RegionMapEditorIO.cpp
 * @brief Load / save / serialize for RegionMapEditorPanel continent region maps.
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
 * Saving backs up the selected region map to a sibling .bak file first, then
 * re-reads the written file
 * through Json::ParseStrict; on failure the backup is restored.
 */

#include "RegionMapEditorPanel.h"

#include "RegionMapEditorInternal.h"
#include "Utils/EditorProcessLaunch.h"
#include "Utils/JsonUtils.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace SparkEditor
{
    using namespace RegionMapInternal;

    namespace
    {
        constexpr const char* kDefaultDataFile = "regions.json";

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
    } // namespace

    // ========================================================================
    // Load / save
    // ========================================================================

    void RegionMapEditorPanel::ResolveDataPath()
    {
        // Probe both cwd and the real executable location. Shell/app launchers
        // commonly assign an unrelated cwd, while installed/build-tree editors
        // always have a stable executable path.
        std::vector<fs::path> roots;
        auto addAncestors = [&roots](fs::path current)
        {
            for (int depth = 0; !current.empty() && depth < 8; ++depth)
            {
                roots.push_back(current);
                const fs::path parent = current.parent_path();
                if (parent == current)
                    break;
                current = parent;
            }
        };
        std::error_code ec;
        addAncestors(fs::current_path(ec));
        const std::string executableDirectory = GetEditorExecutableDirectory();
        if (!executableDirectory.empty())
            addAncestors(fs::path(executableDirectory));

        auto findDataDirectory = [&roots, &ec](bool requireSourceRoot)
        {
            for (const fs::path& root : roots)
            {
                if (requireSourceRoot &&
                    (!fs::is_regular_file(root / "CMakeLists.txt", ec) || !fs::is_directory(root / "SparkEditor", ec)))
                {
                    ec.clear();
                    continue;
                }
                const fs::path candidate = root / "Assets" / "MMOFPS" / "Data";
                if (fs::is_directory(candidate, ec) && !ec)
                    return fs::absolute(candidate, ec).lexically_normal();
                ec.clear();
            }
            return fs::path{};
        };
        // A build tree can contain copied runtime assets. Prefer the source
        // root so authoring never silently saves into build/bin/<config>.
        fs::path dataDirectory = findDataDirectory(true);
        if (dataDirectory.empty())
            dataDirectory = findDataDirectory(false);
        if (dataDirectory.empty())
            dataDirectory = fs::absolute(fs::path("Assets") / "MMOFPS" / "Data", ec).lexically_normal();

        if (LoadRegionMapDataSources(dataDirectory, m_dataSources, m_dataSourceError))
        {
            m_dataSourceIndex = 0;
            for (size_t i = 0; i < m_dataSources.size(); ++i)
            {
                if (m_dataSources[i].key == "cindral_wastes")
                {
                    m_dataSourceIndex = static_cast<int>(i);
                    break;
                }
            }
            m_dataPath = m_dataSources[static_cast<size_t>(m_dataSourceIndex)].dataPath.generic_string();
        }
        else
        {
            m_dataSourceIndex = -1;
            m_dataPath = (dataDirectory / kDefaultDataFile).generic_string();
        }
    }

    bool RegionMapEditorPanel::SelectDataSource(size_t index, std::string& outError)
    {
        if (index >= m_dataSources.size())
        {
            outError = "invalid continent selection";
            return false;
        }
        m_dataSourceIndex = static_cast<int>(index);
        m_dataPath = m_dataSources[index].dataPath.generic_string();
        return LoadFromDisk(outError);
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
        // 1) Backup the current selected region map (if any) to a sibling .bak file.
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

} // namespace SparkEditor
