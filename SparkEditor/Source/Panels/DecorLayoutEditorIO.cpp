/**
 * @file DecorLayoutEditorIO.cpp
 * @brief Load/save/serialize for the decor layout editor (Assets/MMOFPS/Data/decor.json)
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains: ResolveDataPath, LoadFromDisk, SerializeDocument, SaveToDisk.
 *
 * Schema contract — the writer emits EXACTLY the fields consumed by the game
 * module's TFRegionDecor::LoadTemplates (GameModules/SparkGameMMOFPS/Source/
 * World/TFRegionDecor.cpp), in a stable order:
 *
 *   root:     "$schema_note" (preserved verbatim), "clearanceM", "templates"
 *   template: tier keys in the fixed order outpost, fort, facility, skyanchor
 *             (only tiers that exist), each { "pieces", "scatter" }
 *   piece:    "model", "collide" (only when true AND no parts — authoring
 *             collideParts implies collide in the loader), "offset" [x,z],
 *             "yaw", "terrainAlign" (only when false), "material" (only when
 *             set), "emissive" (only when != 0), "castShadows" (only when
 *             false), "collideParts" (only when non-empty)
 *   part:     "offset" [x,y,z], "size" [x,y,z], "yawDeg" (only when != 0)
 *   scatter:  "models", "count" [min,max], "radius" [min,max] (only when the
 *             tier authored any scatter)
 *
 * Unknown/extra keys in a hand-edited file are NOT round-tripped — saving
 * normalizes the file to the schema above (the RegionMapEditorPanel house
 * rule: Spark::Json::Value objects are unordered, so the writer is hand-
 * rolled for a stable field order).
 *
 * Saving backs up to decor.json.bak first, then re-reads the written file
 * through Json::ParseStrict; on failure the backup is restored.
 */

#include "DecorLayoutEditorPanel.h"

#include "DecorLayoutEditorInternal.h"
#include "Utils/JsonUtils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace SparkEditor
{

    namespace
    {
        constexpr const char* kDataRelPath = "Assets/MMOFPS/Data/decor.json";

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

        // Lenient JSON getters (load must tolerate broken files so the user can
        // FIX them in the editor; RunValidation reports what the game degrades).
        std::string GetStr(const Spark::Json::Value& o, const char* key, const char* fallback = "")
        {
            return (o.HasKey(key) && o[key].IsString()) ? o[key].AsString() : std::string(fallback);
        }

        float GetNum(const Spark::Json::Value& o, const char* key, float fallback)
        {
            return (o.HasKey(key) && o[key].IsNumber()) ? static_cast<float>(o[key].AsNumber()) : fallback;
        }

        bool GetBool(const Spark::Json::Value& o, const char* key, bool fallback)
        {
            return o.HasKey(key) ? o[key].AsBool(fallback) : fallback;
        }
    } // namespace

    // ========================================================================
    // Load / save
    // ========================================================================

    void DecorLayoutEditorPanel::ResolveDataPath()
    {
        // The editor normally runs with the repo root as cwd, but probe a few
        // parents so the panel also works when launched from a build output
        // directory (same probe as RegionMapEditorPanel / SceneImportPanel).
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

    bool DecorLayoutEditorPanel::LoadFromDisk(std::string& outError)
    {
        m_loaded = false;
        m_dirty = false;
        m_schemaNote.clear();
        m_clearanceM = 8.0f;
        for (TierTemplate& t : m_tiers)
            t = TierTemplate{};
        m_boundsCache.clear(); // a Reload should also pick up fixed/added OBJs
        m_selPiece = -1;
        m_selPart = -1;
        m_bufTier = -1;
        m_bufPiece = -1;
        m_dragKind = DragKind::None;
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
        m_clearanceM = GetNum(root, "clearanceM", 8.0f);

        int pieceTotal = 0;
        if (root.HasKey("templates") && root["templates"].IsObject())
        {
            const Spark::Json::Value& templates = root["templates"];
            for (int ti = 0; ti < kTierCount; ++ti)
            {
                if (!templates.HasKey(kTierNames[ti]) || !templates[kTierNames[ti]].IsObject())
                    continue;
                const Spark::Json::Value& t = templates[kTierNames[ti]];
                TierTemplate& out = m_tiers[static_cast<size_t>(ti)];
                out.present = true;

                if (t.HasKey("pieces") && t["pieces"].IsArray())
                {
                    const Spark::Json::Value& pieces = t["pieces"];
                    for (size_t i = 0; i < pieces.Size(); ++i)
                    {
                        const Spark::Json::Value& p = pieces[i];
                        if (!p.IsObject())
                            continue;
                        Piece piece;
                        piece.model = GetStr(p, "model");
                        piece.material = GetStr(p, "material");
                        if (p.HasKey("offset") && p["offset"].IsArray() && p["offset"].Size() >= 2)
                        {
                            piece.offX = static_cast<float>(p["offset"][0].AsNumber(0.0));
                            piece.offZ = static_cast<float>(p["offset"][1].AsNumber(0.0));
                        }
                        piece.yawDeg = GetNum(p, "yaw", 0.0f);
                        piece.terrainAlign = GetBool(p, "terrainAlign", true);
                        piece.castShadows = GetBool(p, "castShadows", true);
                        piece.emissive = GetNum(p, "emissive", 0.0f);
                        piece.collide = GetBool(p, "collide", false);
                        if (p.HasKey("collideParts") && p["collideParts"].IsArray())
                        {
                            const Spark::Json::Value& parts = p["collideParts"];
                            for (size_t k = 0; k < parts.Size(); ++k)
                            {
                                const Spark::Json::Value& pv = parts[k];
                                if (!pv.IsObject())
                                    continue;
                                CollidePart part;
                                if (pv.HasKey("offset") && pv["offset"].IsArray() && pv["offset"].Size() >= 3)
                                    for (size_t a = 0; a < 3; ++a)
                                        part.off[a] = static_cast<float>(pv["offset"][a].AsNumber(0.0));
                                if (pv.HasKey("size") && pv["size"].IsArray() && pv["size"].Size() >= 3)
                                    for (size_t a = 0; a < 3; ++a)
                                        part.size[a] = static_cast<float>(pv["size"][a].AsNumber(1.0));
                                part.yawDeg = GetNum(pv, "yawDeg", 0.0f);
                                piece.collideParts.push_back(part);
                            }
                            if (!piece.collideParts.empty())
                                piece.collide = true; // loader rule: parts imply collide
                        }
                        out.pieces.push_back(std::move(piece));
                        ++pieceTotal;
                    }
                }

                if (t.HasKey("scatter") && t["scatter"].IsObject())
                {
                    const Spark::Json::Value& s = t["scatter"];
                    Scatter& sc = out.scatter;
                    if (s.HasKey("models") && s["models"].IsArray())
                        for (size_t i = 0; i < s["models"].Size(); ++i)
                            sc.models.push_back(s["models"][i].AsString(std::string()));
                    if (s.HasKey("count") && s["count"].IsArray() && s["count"].Size() >= 2)
                    {
                        sc.countMin = s["count"][0].AsInt(0);
                        sc.countMax = s["count"][1].AsInt(sc.countMin);
                    }
                    if (s.HasKey("radius") && s["radius"].IsArray() && s["radius"].Size() >= 2)
                    {
                        sc.radiusMin = static_cast<float>(s["radius"][0].AsNumber(0.0));
                        sc.radiusMax = static_cast<float>(s["radius"][1].AsNumber(static_cast<double>(sc.radiusMin)));
                    }
                }
            }
        }

        m_loaded = true;
        m_statusMsg = "Loaded " + std::to_string(pieceTotal) + " pieces across " +
                      std::to_string(std::count_if(m_tiers.begin(), m_tiers.end(),
                                                   [](const TierTemplate& t) { return t.present; })) +
                      " tier templates.";
        m_statusIsError = false;
        return true;
    }

    std::string DecorLayoutEditorPanel::SerializeDocument() const
    {
        std::string out;
        out.reserve(8192);
        out += "{\n";

        if (!m_schemaNote.empty())
        {
            out += "    \"$schema_note\": ";
            AppendEscaped(out, m_schemaNote);
            out += ",\n";
        }
        out += "    \"clearanceM\": " + FormatNum(m_clearanceM) + ",\n";

        out += "    \"templates\": {\n";
        std::vector<int> present;
        for (int ti = 0; ti < kTierCount; ++ti)
            if (m_tiers[static_cast<size_t>(ti)].present)
                present.push_back(ti);
        for (size_t pi = 0; pi < present.size(); ++pi)
        {
            const int ti = present[pi];
            const TierTemplate& t = m_tiers[static_cast<size_t>(ti)];
            out += "        \"";
            out += kTierNames[ti];
            out += "\": {\n";

            out += "            \"pieces\": [";
            for (size_t i = 0; i < t.pieces.size(); ++i)
            {
                const Piece& p = t.pieces[i];
                out += (i == 0) ? "\n" : ",\n";
                out += "                { \"model\": ";
                AppendEscaped(out, p.model);
                if (p.collide && p.collideParts.empty())
                    out += ", \"collide\": true";
                out += ", \"offset\": [" + FormatNum(p.offX) + ", " + FormatNum(p.offZ) + "]";
                out += ", \"yaw\": " + FormatNum(p.yawDeg);
                if (!p.terrainAlign)
                    out += ", \"terrainAlign\": false";
                if (!p.material.empty())
                {
                    out += ", \"material\": ";
                    AppendEscaped(out, p.material);
                }
                if (p.emissive != 0.0f)
                    out += ", \"emissive\": " + FormatNum(p.emissive);
                if (!p.castShadows)
                    out += ", \"castShadows\": false";
                if (!p.collideParts.empty())
                {
                    out += ", \"collideParts\": [\n";
                    for (size_t k = 0; k < p.collideParts.size(); ++k)
                    {
                        const CollidePart& part = p.collideParts[k];
                        out += "                    { \"offset\": [" + FormatNum(part.off[0]) + ", " +
                               FormatNum(part.off[1]) + ", " + FormatNum(part.off[2]) + "], \"size\": [" +
                               FormatNum(part.size[0]) + ", " + FormatNum(part.size[1]) + ", " +
                               FormatNum(part.size[2]) + "]";
                        if (part.yawDeg != 0.0f)
                            out += ", \"yawDeg\": " + FormatNum(part.yawDeg);
                        out += " }";
                        out += (k + 1 < p.collideParts.size()) ? ",\n" : "\n";
                    }
                    out += "                ]";
                }
                out += " }";
            }
            out += t.pieces.empty() ? "]" : "\n            ]";

            const Scatter& sc = t.scatter;
            const bool hasScatter = !sc.models.empty() || sc.countMax > 0;
            if (hasScatter)
            {
                out += ",\n            \"scatter\": {\n";
                out += "                \"models\": [";
                for (size_t i = 0; i < sc.models.size(); ++i)
                {
                    out += (i == 0) ? "\n" : ",\n";
                    out += "                    ";
                    AppendEscaped(out, sc.models[i]);
                }
                out += sc.models.empty() ? "],\n" : "\n                ],\n";
                out += "                \"count\": [" + std::to_string(sc.countMin) + ", " +
                       std::to_string(sc.countMax) + "],\n";
                out +=
                    "                \"radius\": [" + FormatNum(sc.radiusMin) + ", " + FormatNum(sc.radiusMax) + "]\n";
                out += "            }";
            }
            out += "\n        }";
            out += (pi + 1 < present.size()) ? ",\n" : "\n";
        }
        out += "    }\n";

        out += "}\n";
        return out;
    }

    bool DecorLayoutEditorPanel::SaveToDisk(std::string& outError)
    {
        // 1) Backup the current file (if any) to decor.json.bak.
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
