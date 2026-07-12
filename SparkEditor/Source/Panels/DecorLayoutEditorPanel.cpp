/**
 * @file DecorLayoutEditorPanel.cpp
 * @brief Visual editor for Assets/MMOFPS/Data/decor.json (W12)
 * @author Spark Engine Team
 * @date 2026
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
 * through Json::ParseStrict; on failure the backup is restored. Validation
 * (missing OBJs, part budget kMaxCollideParts, the 20-per-region piece cap,
 * degenerate part sizes, scatter range sanity) blocks the save until the
 * 'I know what I am doing' checkbox is ticked.
 *
 * Geometry conventions mirror the game exactly:
 *  - offset = [x, z] meters from region center (+x east, +z north); yaw in
 *    DEGREES (the RADIANS rule is PhysicsBody-only).
 *  - Footprints come from streaming the OBJ vertex min/max, the same contract
 *    as TFWorldCollision::ObjLocalAabb (local reader — no module linkage).
 *  - The yaw mapping is the play-test-validated Build() convention:
 *    x' = x*c + z*s, z' = -x*s + z*c; part world center = piece origin +
 *    Ry(pieceYaw) * part offset, part box rotation = pieceYaw + partYaw.
 *
 * This panel edits the DATA FILE only — no live world interaction.
 */

#include "DecorLayoutEditorPanel.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace SparkEditor
{

    namespace
    {
        // Mirrors of the game-module limits/conventions. The editor cannot
        // include game-module headers, so the values are duplicated here; keep
        // them in sync with their sources:
        //  - kMaxCollideParts / kMaxDecorPerRegion: World/TFRegionDecor.cpp
        //  - kPawnRadiusM: Game/TFMovementModel.h (kTFPawnRadiusM 0.4 => 0.8 m diameter)
        constexpr size_t kMaxCollideParts = 5;
        constexpr size_t kMaxDecorPerRegion = 20;
        constexpr float kPawnRadiusM = 0.4f;

        constexpr const char* kDataRelPath = "Assets/MMOFPS/Data/decor.json";

        /// Fixed tier order — matches both the hand-authored file and the
        /// kTiers iteration in TFRegionDecor::LoadTemplates.
        const char* const kTierNames[] = {"outpost", "fort", "facility", "skyanchor"};
        constexpr int kTierCount = 4;

        constexpr float kDegToRad = 0.01745329252f; // same constant as TFWorldCollision

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

        /// Short display label: path basename without the .obj suffix.
        std::string ModelLabel(const std::string& model)
        {
            const size_t slash = model.find_last_of('/');
            std::string base = (slash == std::string::npos) ? model : model.substr(slash + 1);
            if (base.size() > 4 && base.compare(base.size() - 4, 4, ".obj") == 0)
                base.resize(base.size() - 4);
            return base;
        }

        float Dist2(float ax, float ay, float bx, float by)
        {
            const float dx = ax - bx;
            const float dy = ay - by;
            return dx * dx + dy * dy;
        }

        /// Snap to 15-degree steps, normalized to (-180, 180].
        float SnapYaw15(float deg)
        {
            float snapped = std::round(deg / 15.0f) * 15.0f;
            while (snapped > 180.0f)
                snapped -= 360.0f;
            while (snapped <= -180.0f)
                snapped += 360.0f;
            return snapped;
        }
    } // namespace

    // ========================================================================
    // Construction / lifecycle
    // ========================================================================

    DecorLayoutEditorPanel::DecorLayoutEditorPanel() : EditorPanel("Decor Layout Editor", "decor_layout_editor_panel")
    {
        m_category = PanelCategory::Tool;
    }

    bool DecorLayoutEditorPanel::Initialize()
    {
        ResolveDataPath();
        std::string err;
        if (LoadFromDisk(err))
        {
            int pieces = 0;
            for (const TierTemplate& t : m_tiers)
                pieces += static_cast<int>(t.pieces.size());
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "DecorLayoutEditorPanel: loaded %d piece(s) from '%s'", pieces,
                           m_dataPath.c_str());
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "DecorLayoutEditorPanel: failed to load '%s': %s",
                           m_dataPath.c_str(), err.c_str());
        }
        return true; // the panel is still useful (shows the error + Reload button)
    }

    void DecorLayoutEditorPanel::Update(float /*deltaTime*/) {}

    void DecorLayoutEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Decor Layout Editor panel");
    }

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

    // ========================================================================
    // OBJ footprints
    // ========================================================================

    const DecorLayoutEditorPanel::ObjBounds& DecorLayoutEditorPanel::BoundsForModel(const std::string& model)
    {
        auto it = m_boundsCache.find(model);
        if (it != m_boundsCache.end())
            return it->second;

        // Stream the OBJ once — vertex lines only, min/max accumulate. Same
        // contract as TFWorldCollision::ObjLocalAabb (which is module code the
        // editor must not link). Misses are cached too, so a missing file does
        // not re-hit the disk every frame; Reload clears the cache.
        ObjBounds b;
        std::ifstream f(m_assetsPrefix + model);
        if (f.is_open())
        {
            bool any = false;
            std::string line;
            while (std::getline(f, line))
            {
                if (line.size() < 3 || line[0] != 'v' || line[1] != ' ')
                    continue;
                float v[3] = {0.0f, 0.0f, 0.0f};
                const char* c = line.c_str() + 2;
                for (float& axis : v)
                {
                    char* end = nullptr;
                    axis = std::strtof(c, &end);
                    if (end == c)
                        break;
                    c = end;
                }
                if (!any)
                {
                    for (size_t i = 0; i < 3; ++i)
                        b.mn[i] = b.mx[i] = v[i];
                    any = true;
                }
                else
                {
                    for (size_t i = 0; i < 3; ++i)
                    {
                        b.mn[i] = std::min(b.mn[i], v[i]);
                        b.mx[i] = std::max(b.mx[i], v[i]);
                    }
                }
            }
            b.valid = any;
        }
        if (!b.valid)
        {
            b.mn = {-0.5f, -0.5f, -0.5f}; // unit-cube fallback, drawn dashed
            b.mx = {0.5f, 0.5f, 0.5f};
        }
        return m_boundsCache.emplace(model, b).first->second;
    }

    // ========================================================================
    // Validation
    // ========================================================================

    void DecorLayoutEditorPanel::RunValidation()
    {
        m_violations.clear();
        m_validationRan = true;
        if (!m_loaded)
        {
            m_violations.push_back("no document loaded");
            return;
        }
        if (m_clearanceM <= 0.0f)
            m_violations.push_back("clearanceM must be positive");

        bool anyPresent = false;
        for (int ti = 0; ti < kTierCount; ++ti)
        {
            const TierTemplate& t = m_tiers[static_cast<size_t>(ti)];
            if (!t.present)
                continue;
            anyPresent = true;
            const std::string tier = kTierNames[ti];

            for (size_t i = 0; i < t.pieces.size(); ++i)
            {
                const Piece& p = t.pieces[i];
                const std::string tag = tier + " piece " + std::to_string(i) + " ('" + ModelLabel(p.model) + "')";
                if (p.model.empty())
                {
                    m_violations.push_back(tag + ": empty model path (the game skips it)");
                    continue;
                }
                if (!BoundsForModel(p.model).valid)
                    m_violations.push_back(tag + ": OBJ missing/unreadable ('" + p.model +
                                           "') - placeholder visual, collidable pieces stay walk-through");
                if (p.collideParts.size() > kMaxCollideParts)
                    m_violations.push_back(tag + ": " + std::to_string(p.collideParts.size()) +
                                           " collideParts exceeds the game cap of " + std::to_string(kMaxCollideParts) +
                                           " (extras dropped)");
                for (size_t k = 0; k < p.collideParts.size(); ++k)
                {
                    const CollidePart& part = p.collideParts[k];
                    if (part.size[0] <= 0.0f || part.size[1] <= 0.0f || part.size[2] <= 0.0f)
                        m_violations.push_back(tag + ": collidePart " + std::to_string(k) +
                                               " has a non-positive size axis");
                }
            }

            const Scatter& sc = t.scatter;
            if (sc.countMin < 0)
                m_violations.push_back(tier + ": scatter count min is negative");
            if (sc.countMax < sc.countMin)
                m_violations.push_back(tier + ": scatter count max < min (the game clamps, fix the data)");
            if (sc.radiusMax < sc.radiusMin)
                m_violations.push_back(tier + ": scatter radius max < min (the game clamps, fix the data)");
            if (sc.countMax > 0 && sc.models.empty())
                m_violations.push_back(tier + ": scatter count > 0 but no scatter models");
            for (size_t i = 0; i < sc.models.size(); ++i)
            {
                if (sc.models[i].empty())
                    m_violations.push_back(tier + ": scatter model " + std::to_string(i) + " is empty");
                else if (!BoundsForModel(sc.models[i]).valid)
                    m_violations.push_back(tier + ": scatter model OBJ missing ('" + sc.models[i] + "')");
            }

            const size_t worstCase = t.pieces.size() + static_cast<size_t>(std::max(sc.countMax, 0));
            if (worstCase > kMaxDecorPerRegion)
                m_violations.push_back(tier + ": pieces + max scatter = " + std::to_string(worstCase) +
                                       " exceeds the per-region cap of " + std::to_string(kMaxDecorPerRegion) +
                                       " (the game drops overflow)");
        }
        if (!anyPresent)
            m_violations.push_back("no tier templates - every region stays bare");
    }

    // ========================================================================
    // Editing helpers
    // ========================================================================

    void DecorLayoutEditorPanel::SelectPiece(int piece, int part)
    {
        m_selPiece = piece;
        m_selPart = part;
        SyncEditBuffers();
    }

    void DecorLayoutEditorPanel::SyncEditBuffers()
    {
        if (m_selPiece == m_bufPiece && m_tier == m_bufTier)
            return;
        m_bufTier = m_tier;
        m_bufPiece = m_selPiece;
        const TierTemplate& t = m_tiers[static_cast<size_t>(m_tier)];
        if (m_selPiece >= 0 && m_selPiece < static_cast<int>(t.pieces.size()))
        {
            const Piece& p = t.pieces[static_cast<size_t>(m_selPiece)];
            std::snprintf(m_modelBuf, sizeof(m_modelBuf), "%s", p.model.c_str());
            std::snprintf(m_materialBuf, sizeof(m_materialBuf), "%s", p.material.c_str());
        }
    }

    void DecorLayoutEditorPanel::MarkDirty()
    {
        m_dirty = true;
        m_validationRan = false;
    }

    // ========================================================================
    // Rendering
    // ========================================================================

    void DecorLayoutEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            if (ImGui::BeginTabBar("decor_tier_tabs"))
            {
                for (int ti = 0; ti < kTierCount; ++ti)
                {
                    if (ImGui::BeginTabItem(kTierNames[ti]))
                    {
                        if (m_tier != ti)
                        {
                            m_tier = ti;
                            SelectPiece(-1, -1);
                            m_dragKind = DragKind::None;
                            m_fitRequested = true;
                        }
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }

            TierTemplate& t = m_tiers[static_cast<size_t>(m_tier)];

            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float sideW = 380.0f;
            const float canvasW = std::max(140.0f, avail.x - sideW - 8.0f);

            ImGui::BeginChild("decor_canvas_child", ImVec2(canvasW, 0.0f), true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            RenderCanvas(t);
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("decor_side_pane", ImVec2(0.0f, 0.0f), true);
            RenderSidePane(t);
            ImGui::EndChild();
        }
        EndPanel();
    }

    void DecorLayoutEditorPanel::RenderToolbar()
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
        ImGui::Checkbox("Clearance overlay", &m_showClearance);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draggable probe with the %.0fm capture-point clearance circle and the 0.8m pawn "
                              "capsule diameter, so walkable gaps are visible at a glance.",
                              static_cast<double>(m_clearanceM));
        ImGui::SameLine();
        ImGui::TextDisabled("| %s%s", m_dataPath.c_str(), m_dirty ? " *" : "");
    }

    void DecorLayoutEditorPanel::RenderCanvas(TierTemplate& t)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        size.x = std::max(size.x, 64.0f);
        size.y = std::max(size.y, 64.0f);
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);

        ImGui::InvisibleButton("decor_layout_canvas", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImGuiIO& io = ImGui::GetIO();

        if (m_fitRequested)
        {
            // Half-extent covering every footprint corner + scatter ring + the
            // clearance circle, with padding; minimum keeps tiny templates sane.
            float extent = std::max(m_clearanceM + 4.0f, 24.0f);
            for (const Piece& p : t.pieces)
            {
                const ObjBounds& b = BoundsForModel(p.model);
                const float reach =
                    std::sqrt(Dist2(p.offX, p.offZ, 0.0f, 0.0f)) +
                    std::max({std::fabs(b.mn[0]), std::fabs(b.mx[0]), std::fabs(b.mn[2]), std::fabs(b.mx[2])});
                extent = std::max(extent, reach + 6.0f);
            }
            extent = std::max(extent, t.scatter.radiusMax + 6.0f);
            m_viewExtent = extent;
            m_zoom = 1.0f;
            m_panX = 0.0f;
            m_panY = 0.0f;
            m_fitRequested = false;
        }

        const float baseScale = 0.5f * std::min(size.x, size.y) / std::max(1.0f, m_viewExtent);
        const ImVec2 ctrRaw(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);

        // Zoom around the mouse cursor.
        if (hovered && io.MouseWheel != 0.0f)
        {
            const float sOld = baseScale * m_zoom;
            const float newZoom = std::clamp(m_zoom * std::pow(1.15f, io.MouseWheel), 0.1f, 40.0f);
            const float sNew = baseScale * newZoom;
            const float relX = io.MousePos.x - ctrRaw.x;
            const float relY = io.MousePos.y - ctrRaw.y;
            m_panX = relX - (relX - m_panX) * (sNew / sOld);
            m_panY = relY - (relY - m_panY) * (sNew / sOld);
            m_zoom = newZoom;
        }
        const float s = baseScale * m_zoom;

        auto toScreen = [&](float wx, float wz)
        { return ImVec2(ctrRaw.x + m_panX + wx * s, ctrRaw.y + m_panY - wz * s); };
        auto toWorldX = [&](float sx) { return (sx - ctrRaw.x - m_panX) / s; };
        auto toWorldZ = [&](float sy) { return -(sy - ctrRaw.y - m_panY) / s; };

        // Pan with middle or right drag.
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                       ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)))
        {
            m_panX += io.MouseDelta.x;
            m_panY += io.MouseDelta.y;
        }

        // Footprint helpers — the game's yaw mapping (x' = x*c + z*s, z' = -x*s + z*c).
        auto pieceCorners = [&](const Piece& p, const ObjBounds& b, ImVec2 out[4])
        {
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            const float xs[4] = {b.mn[0], b.mx[0], b.mx[0], b.mn[0]};
            const float zs[4] = {b.mn[2], b.mn[2], b.mx[2], b.mx[2]};
            for (int i = 0; i < 4; ++i)
            {
                const float wx = xs[i] * c + zs[i] * sn + p.offX;
                const float wz = -xs[i] * sn + zs[i] * c + p.offZ;
                out[i] = toScreen(wx, wz);
            }
        };
        auto pointInPiece = [&](const Piece& p, const ObjBounds& b, float wx, float wz)
        {
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            const float dx = wx - p.offX;
            const float dz = wz - p.offZ;
            const float lx = c * dx - sn * dz; // inverse of the forward mapping
            const float lz = sn * dx + c * dz;
            const float pad = 4.0f / s; // few pixels of grab slack
            return lx >= b.mn[0] - pad && lx <= b.mx[0] + pad && lz >= b.mn[2] - pad && lz <= b.mx[2] + pad;
        };
        auto partCenterWorld = [&](const Piece& p, const CollidePart& part, float& wx, float& wz)
        {
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            wx = part.off[0] * c + part.off[2] * sn + p.offX;
            wz = -part.off[0] * sn + part.off[2] * c + p.offZ;
        };
        auto partCorners = [&](const Piece& p, const CollidePart& part, ImVec2 out[4])
        {
            float cx = 0.0f, cz = 0.0f;
            partCenterWorld(p, part, cx, cz);
            const float total = (p.yawDeg + part.yawDeg) * kDegToRad;
            const float c = std::cos(total);
            const float sn = std::sin(total);
            const float hx = part.size[0] * 0.5f;
            const float hz = part.size[2] * 0.5f;
            const float xs[4] = {-hx, hx, hx, -hx};
            const float zs[4] = {-hz, -hz, hz, hz};
            for (int i = 0; i < 4; ++i)
            {
                const float wx = xs[i] * c + zs[i] * sn + cx;
                const float wz = -xs[i] * sn + zs[i] * c + cz;
                out[i] = toScreen(wx, wz);
            }
        };
        auto partResizeHandle = [&](const Piece& p, const CollidePart& part)
        {
            float cx = 0.0f, cz = 0.0f;
            partCenterWorld(p, part, cx, cz);
            const float total = (p.yawDeg + part.yawDeg) * kDegToRad;
            const float c = std::cos(total);
            const float sn = std::sin(total);
            const float hx = part.size[0] * 0.5f;
            const float hz = part.size[2] * 0.5f;
            return toScreen(hx * c + hz * sn + cx, -hx * sn + hz * c + cz); // local (+hx, +hz) corner
        };
        auto rotateHandle = [&](const Piece& p, const ObjBounds& b)
        {
            const float reach = std::max(std::fabs(b.mn[2]), std::fabs(b.mx[2])) + 16.0f / s;
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            // Local +z rotated: R(yaw) * (0, reach) = (reach*sn, reach*c).
            return toScreen(p.offX + reach * sn, p.offZ + reach * c);
        };

        Piece* sel = (m_selPiece >= 0 && m_selPiece < static_cast<int>(t.pieces.size()))
                         ? &t.pieces[static_cast<size_t>(m_selPiece)]
                         : nullptr;

        // ---- Left-mouse interaction: hit test on press, drag while held ----
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_dragKind = DragKind::None;
            const float hitR2 = 8.0f * 8.0f;
            const float mwx = toWorldX(io.MousePos.x);
            const float mwz = toWorldZ(io.MousePos.y);

            // 1) Clearance probe (small, sits on top).
            if (m_showClearance)
            {
                const ImVec2 pp = toScreen(m_probeX, m_probeZ);
                if (Dist2(pp.x, pp.y, io.MousePos.x, io.MousePos.y) < hitR2)
                    m_dragKind = DragKind::Probe;
            }
            // 2) Selected piece's handles + parts.
            if (m_dragKind == DragKind::None && sel != nullptr)
            {
                const ObjBounds& b = BoundsForModel(sel->model);
                const ImVec2 rh = rotateHandle(*sel, b);
                if (Dist2(rh.x, rh.y, io.MousePos.x, io.MousePos.y) < hitR2)
                {
                    m_dragKind = DragKind::PieceRotate;
                }
                else
                {
                    for (size_t k = 0; k < sel->collideParts.size() && m_dragKind == DragKind::None; ++k)
                    {
                        const CollidePart& part = sel->collideParts[k];
                        const ImVec2 rz = partResizeHandle(*sel, part);
                        if (Dist2(rz.x, rz.y, io.MousePos.x, io.MousePos.y) < hitR2)
                        {
                            m_selPart = static_cast<int>(k);
                            m_dragKind = DragKind::PartResize;
                            break;
                        }
                        float cx = 0.0f, cz = 0.0f;
                        partCenterWorld(*sel, part, cx, cz);
                        const float total = (sel->yawDeg + part.yawDeg) * kDegToRad;
                        const float c = std::cos(total);
                        const float sn = std::sin(total);
                        const float dx = mwx - cx;
                        const float dz = mwz - cz;
                        const float lx = c * dx - sn * dz;
                        const float lz = sn * dx + c * dz;
                        if (std::fabs(lx) <= part.size[0] * 0.5f && std::fabs(lz) <= part.size[2] * 0.5f)
                        {
                            m_selPart = static_cast<int>(k);
                            m_dragKind = DragKind::PartMove;
                        }
                    }
                }
            }
            // 3) Piece bodies (topmost drawn = last in the list, so hit-test back to front).
            if (m_dragKind == DragKind::None)
            {
                int hit = -1;
                for (int i = static_cast<int>(t.pieces.size()) - 1; i >= 0; --i)
                {
                    const Piece& p = t.pieces[static_cast<size_t>(i)];
                    if (pointInPiece(p, BoundsForModel(p.model), mwx, mwz))
                    {
                        hit = i;
                        break;
                    }
                }
                if (hit >= 0)
                {
                    SelectPiece(hit, -1);
                    m_dragKind = DragKind::PieceMove;
                }
                else
                {
                    SelectPiece(-1, -1);
                }
            }
            // The click may have changed the selection — refresh the pointer so
            // a same-frame drag delta moves the piece that was just grabbed.
            sel = (m_selPiece >= 0 && m_selPiece < static_cast<int>(t.pieces.size()))
                      ? &t.pieces[static_cast<size_t>(m_selPiece)]
                      : nullptr;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_dragKind = DragKind::None;
        }
        else if (active && m_dragKind != DragKind::None && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
        {
            const float dwx = io.MouseDelta.x / s;
            const float dwz = -io.MouseDelta.y / s;
            switch (m_dragKind)
            {
            case DragKind::Probe:
                m_probeX += dwx;
                m_probeZ += dwz;
                break;
            case DragKind::PieceMove:
                if (sel != nullptr)
                {
                    sel->offX += dwx;
                    sel->offZ += dwz;
                    MarkDirty();
                }
                break;
            case DragKind::PieceRotate:
                if (sel != nullptr)
                {
                    const float dx = toWorldX(io.MousePos.x) - sel->offX;
                    const float dz = toWorldZ(io.MousePos.y) - sel->offZ;
                    if (dx != 0.0f || dz != 0.0f)
                    {
                        // Local +z points along (sin yaw, cos yaw) => yaw = atan2(x, z).
                        const float snapped = SnapYaw15(std::atan2(dx, dz) / kDegToRad);
                        if (snapped != sel->yawDeg)
                        {
                            sel->yawDeg = snapped;
                            MarkDirty();
                        }
                    }
                }
                break;
            case DragKind::PartMove:
                if (sel != nullptr && m_selPart >= 0 && m_selPart < static_cast<int>(sel->collideParts.size()))
                {
                    // World delta -> piece-local delta (inverse rotation by piece yaw).
                    CollidePart& part = sel->collideParts[static_cast<size_t>(m_selPart)];
                    const float c = std::cos(sel->yawDeg * kDegToRad);
                    const float sn = std::sin(sel->yawDeg * kDegToRad);
                    part.off[0] += c * dwx - sn * dwz;
                    part.off[2] += sn * dwx + c * dwz;
                    MarkDirty();
                }
                break;
            case DragKind::PartResize:
                if (sel != nullptr && m_selPart >= 0 && m_selPart < static_cast<int>(sel->collideParts.size()))
                {
                    // World delta -> part-local delta; the (+hx,+hz) corner follows
                    // the mouse, so each unit of local delta adds two to full size.
                    CollidePart& part = sel->collideParts[static_cast<size_t>(m_selPart)];
                    const float total = (sel->yawDeg + part.yawDeg) * kDegToRad;
                    const float c = std::cos(total);
                    const float sn = std::sin(total);
                    part.size[0] = std::max(0.1f, part.size[0] + 2.0f * (c * dwx - sn * dwz));
                    part.size[2] = std::max(0.1f, part.size[2] + 2.0f * (sn * dwx + c * dwz));
                    MarkDirty();
                }
                break;
            case DragKind::None:
                break;
            }
        }

        // ---- Drawing ----
        dl->PushClipRect(p0, p1, true);
        dl->AddRectFilled(p0, p1, IM_COL32(24, 26, 30, 255));

        // Meter grid (10 m) + origin axes (the region center).
        {
            const float wx0 = toWorldX(p0.x);
            const float wx1 = toWorldX(p1.x);
            const float wz0 = toWorldZ(p1.y); // bottom of the canvas = lowest z
            const float wz1 = toWorldZ(p0.y);
            const float step = 10.0f;
            for (float g = std::floor(wx0 / step) * step; g <= wx1; g += step)
                dl->AddLine(toScreen(g, wz0), toScreen(g, wz1), IM_COL32(60, 63, 70, 110));
            for (float g = std::floor(wz0 / step) * step; g <= wz1; g += step)
                dl->AddLine(toScreen(wx0, g), toScreen(wx1, g), IM_COL32(60, 63, 70, 110));
            dl->AddLine(toScreen(0.0f, wz0), toScreen(0.0f, wz1), IM_COL32(110, 115, 125, 170));
            dl->AddLine(toScreen(wx0, 0.0f), toScreen(wx1, 0.0f), IM_COL32(110, 115, 125, 170));
        }

        // Scatter ring (radius min/max) — context for where props may land.
        if (!t.scatter.models.empty() && t.scatter.radiusMax > 0.0f)
        {
            const ImVec2 o = toScreen(0.0f, 0.0f);
            dl->AddCircle(o, t.scatter.radiusMin * s, IM_COL32(120, 130, 145, 90), 64, 1.0f);
            dl->AddCircle(o, t.scatter.radiusMax * s, IM_COL32(120, 130, 145, 90), 64, 1.0f);
        }

        // Clearance preview UNDER the pieces: the capture-point clearance circle
        // (clearanceM) + the 0.8 m pawn capsule at the draggable probe.
        if (m_showClearance)
        {
            const ImVec2 pp = toScreen(m_probeX, m_probeZ);
            dl->AddCircleFilled(pp, m_clearanceM * s, IM_COL32(250, 210, 70, 26), 64);
            dl->AddCircle(pp, m_clearanceM * s, IM_COL32(250, 210, 70, 170), 64, 1.5f);
            dl->AddCircle(pp, kPawnRadiusM * s, IM_COL32(90, 220, 235, 220), 32, 1.5f);
            dl->AddCircleFilled(pp, 3.0f, IM_COL32(250, 210, 70, 255));
            char label[96];
            std::snprintf(label, sizeof(label), "clearance %.0fm / capsule 0.8m (drag me)",
                          static_cast<double>(m_clearanceM));
            dl->AddText(ImVec2(pp.x + 8.0f, pp.y - 16.0f), IM_COL32(250, 210, 70, 200), label);
        }

        // Pieces as rotated footprint rectangles. Color code:
        //   red    = collides via the whole-model OBB
        //   orange = collides via authored collideParts (arch/underside walkable)
        //   green  = walk-through (visual only)
        //   dashed-ish gray fill = OBJ missing (unit-cube fallback footprint)
        for (size_t i = 0; i < t.pieces.size(); ++i)
        {
            const Piece& p = t.pieces[i];
            const ObjBounds& b = BoundsForModel(p.model);
            ImVec2 quad[4];
            pieceCorners(p, b, quad);

            const bool hasParts = !p.collideParts.empty();
            ImU32 outline = p.collide ? (hasParts ? IM_COL32(255, 170, 60, 255) : IM_COL32(235, 100, 90, 255))
                                      : IM_COL32(120, 200, 140, 255);
            ImU32 fill = p.collide ? (hasParts ? IM_COL32(255, 170, 60, 34) : IM_COL32(235, 100, 90, 40))
                                   : IM_COL32(120, 200, 140, 28);
            if (!b.valid)
            {
                outline = IM_COL32(220, 70, 70, 255); // alarming: footprint is a guess
                fill = IM_COL32(220, 70, 70, 20);
            }
            const bool isSel = (static_cast<int>(i) == m_selPiece);
            dl->AddConvexPolyFilled(quad, 4, fill);
            dl->AddPolyline(quad, 4, isSel ? IM_COL32(255, 255, 255, 255) : outline, isSel ? 3.0f : 1.5f,
                            ImDrawFlags_Closed);

            // Facing tick: piece origin toward local +z.
            const float c = std::cos(p.yawDeg * kDegToRad);
            const float sn = std::sin(p.yawDeg * kDegToRad);
            const ImVec2 org = toScreen(p.offX, p.offZ);
            const float tickM = std::max(1.5f, (b.mx[2] - b.mn[2]) * 0.25f);
            dl->AddLine(org, toScreen(p.offX + tickM * sn, p.offZ + tickM * c), outline, 1.5f);
            dl->AddCircleFilled(org, 3.0f, outline);

            // Nested collidePart boxes (drawn for every piece; edited on the selected one).
            for (size_t k = 0; k < p.collideParts.size(); ++k)
            {
                ImVec2 pq[4];
                partCorners(p, p.collideParts[k], pq);
                const bool partSel = isSel && static_cast<int>(k) == m_selPart;
                dl->AddConvexPolyFilled(pq, 4, IM_COL32(235, 100, 90, 45));
                dl->AddPolyline(pq, 4, partSel ? IM_COL32(255, 255, 255, 255) : IM_COL32(235, 100, 90, 230),
                                partSel ? 2.5f : 1.5f, ImDrawFlags_Closed);
            }

            const std::string label = ModelLabel(p.model);
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            dl->AddText(ImVec2(org.x - ts.x * 0.5f, org.y + 6.0f), IM_COL32(225, 228, 235, 255), label.c_str());
        }

        // Handles for the selected piece: rotate knob + part resize corners.
        if (sel != nullptr)
        {
            const ObjBounds& b = BoundsForModel(sel->model);
            const ImVec2 rh = rotateHandle(*sel, b);
            const ImVec2 org = toScreen(sel->offX, sel->offZ);
            dl->AddLine(org, rh, IM_COL32(255, 255, 120, 160), 1.0f);
            dl->AddCircleFilled(rh, 5.0f, IM_COL32(255, 255, 120, 255));
            dl->AddCircle(rh, 5.0f, IM_COL32(30, 30, 30, 255));
            for (const CollidePart& part : sel->collideParts)
            {
                const ImVec2 rz = partResizeHandle(*sel, part);
                dl->AddRectFilled(ImVec2(rz.x - 4.0f, rz.y - 4.0f), ImVec2(rz.x + 4.0f, rz.y + 4.0f),
                                  IM_COL32(255, 255, 255, 230));
                dl->AddRect(ImVec2(rz.x - 4.0f, rz.y - 4.0f), ImVec2(rz.x + 4.0f, rz.y + 4.0f),
                            IM_COL32(30, 30, 30, 255));
            }
        }

        // Legend / hint line.
        const char* hint =
            sel != nullptr
                ? "Drag body = move, yellow knob = rotate (15-deg steps). Parts: drag = move, white corner = "
                  "resize. Wheel = zoom, right/middle drag = pan."
                : "Click a footprint to select. Red = solid OBB, orange = collideParts, green = walk-through.";
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 4.0f), IM_COL32(170, 175, 185, 255), hint);
        dl->PopClipRect();
    }

    void DecorLayoutEditorPanel::RenderSidePane(TierTemplate& t)
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

        if (m_selPiece >= 0 && m_selPiece < static_cast<int>(t.pieces.size()))
            RenderPieceEditor(t);
        else
            RenderTierEditor(t);

        ImGui::Separator();
        RenderValidationAndSave();
    }

    void DecorLayoutEditorPanel::RenderTierEditor(TierTemplate& t)
    {
        ImGui::Text("Template: %s", kTierNames[m_tier]);
        ImGui::Separator();

        if (!t.present)
        {
            ImGui::TextWrapped("This tier has no template - regions of this tier stay bare.");
            if (ImGui::Button("Create template"))
            {
                t.present = true;
                MarkDirty();
            }
            return;
        }

        if (ImGui::DragFloat("Clearance (m)", &m_clearanceM, 0.25f, 0.5f, 64.0f, "%.1f"))
            MarkDirty();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Root-level clearanceM - the game demotes collidable pieces within this "
                              "distance of any capture point/spawn/terminal to visual-only.");

        ImGui::Spacing();
        ImGui::Text("Pieces (%d)", static_cast<int>(t.pieces.size()));
        for (size_t i = 0; i < t.pieces.size(); ++i)
        {
            const Piece& p = t.pieces[i];
            char row[320];
            std::snprintf(row, sizeof(row), "%d  %s  [%s]##piece%d", static_cast<int>(i), ModelLabel(p.model).c_str(),
                          p.collideParts.empty() ? (p.collide ? "solid" : "walk") : "parts", static_cast<int>(i));
            if (ImGui::Selectable(row, static_cast<int>(i) == m_selPiece))
                SelectPiece(static_cast<int>(i), -1);
        }
        ImGui::InputText("##newmodel", m_newModelBuf, sizeof(m_newModelBuf));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("OBJ path, e.g. Assets/Models/MMOFPS/buildings/barracks.obj");
        ImGui::SameLine();
        if (ImGui::SmallButton("Add piece") && m_newModelBuf[0] != '\0')
        {
            Piece p;
            p.model = m_newModelBuf;
            p.offX = 10.0f; // off-center default so new pieces never crowd the capture point
            p.offZ = 10.0f;
            t.pieces.push_back(std::move(p));
            SelectPiece(static_cast<int>(t.pieces.size()) - 1, -1);
            MarkDirty();
        }

        // Scatter spec.
        ImGui::Spacing();
        ImGui::TextUnformatted("Scatter (seeded prop ring)");
        for (size_t i = 0; i < t.scatter.models.size(); ++i)
        {
            ImGui::PushID(3000 + static_cast<int>(i));
            ImGui::BulletText("%s", ModelLabel(t.scatter.models[i]).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", t.scatter.models[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X##sc"))
            {
                t.scatter.models.erase(t.scatter.models.begin() + static_cast<std::ptrdiff_t>(i));
                MarkDirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::InputText("##newscatter", m_newScatterBuf, sizeof(m_newScatterBuf));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add scatter model") && m_newScatterBuf[0] != '\0')
        {
            t.scatter.models.emplace_back(m_newScatterBuf);
            MarkDirty();
        }
        int count[2] = {t.scatter.countMin, t.scatter.countMax};
        if (ImGui::DragInt2("Count min/max", count, 0.1f, 0, 20))
        {
            t.scatter.countMin = count[0];
            t.scatter.countMax = count[1];
            MarkDirty();
        }
        float radius[2] = {t.scatter.radiusMin, t.scatter.radiusMax};
        if (ImGui::DragFloat2("Radius min/max", radius, 0.5f, 0.0f, 200.0f, "%.0f"))
        {
            t.scatter.radiusMin = radius[0];
            t.scatter.radiusMax = radius[1];
            MarkDirty();
        }

        ImGui::Spacing();
        ImGui::TextWrapped("Select a piece on the canvas (or in the list) to edit it.");
    }

    void DecorLayoutEditorPanel::RenderPieceEditor(TierTemplate& t)
    {
        Piece& p = t.pieces[static_cast<size_t>(m_selPiece)];
        SyncEditBuffers();

        ImGui::Text("%s piece #%d", kTierNames[m_tier], m_selPiece);
        ImGui::SameLine();
        if (ImGui::SmallButton("Back"))
        {
            SelectPiece(-1, -1);
            return;
        }
        ImGui::Separator();

        if (ImGui::InputText("Model", m_modelBuf, sizeof(m_modelBuf)))
        {
            p.model = m_modelBuf;
            MarkDirty();
        }
        {
            const ObjBounds& b = BoundsForModel(p.model);
            if (b.valid)
                ImGui::TextDisabled("footprint %.1f x %.1f m, height %.1f m", static_cast<double>(b.mx[0] - b.mn[0]),
                                    static_cast<double>(b.mx[2] - b.mn[2]), static_cast<double>(b.mx[1] - b.mn[1]));
            else
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "OBJ missing - unit-cube footprint shown");
        }

        float off[2] = {p.offX, p.offZ};
        if (ImGui::DragFloat2("Offset X/Z (m)", off, 0.25f, -300.0f, 300.0f, "%.2f"))
        {
            p.offX = off[0];
            p.offZ = off[1];
            MarkDirty();
        }
        if (ImGui::DragFloat("Yaw (deg)", &p.yawDeg, 1.0f, -180.0f, 180.0f, "%.0f"))
            MarkDirty();
        ImGui::SameLine();
        if (ImGui::SmallButton("Snap 15"))
        {
            const float snapped = SnapYaw15(p.yawDeg);
            if (snapped != p.yawDeg)
            {
                p.yawDeg = snapped;
                MarkDirty();
            }
        }

        if (p.collideParts.empty())
        {
            if (ImGui::Checkbox("Collide (whole-model OBB)", &p.collide))
                MarkDirty();
        }
        else
        {
            ImGui::TextDisabled("Collide: implied by collideParts (whole-model OBB skipped)");
        }
        if (ImGui::Checkbox("Terrain align", &p.terrainAlign))
            MarkDirty();
        ImGui::SameLine();
        if (ImGui::Checkbox("Cast shadows", &p.castShadows))
            MarkDirty();
        if (ImGui::DragFloat("Emissive", &p.emissive, 0.05f, 0.0f, 4.0f, "%.2f"))
            MarkDirty();
        if (ImGui::InputText("Material", m_materialBuf, sizeof(m_materialBuf)))
        {
            p.material = m_materialBuf;
            MarkDirty();
        }

        // Collide parts (W11 gate-passages).
        ImGui::Spacing();
        ImGui::Text("Collide parts (%d/%d)", static_cast<int>(p.collideParts.size()),
                    static_cast<int>(kMaxCollideParts));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Model-local boxes that REPLACE the whole-model OBB - pillars/legs block "
                              "while the archway/underside stays walkable.");
        for (size_t k = 0; k < p.collideParts.size(); ++k)
        {
            CollidePart& part = p.collideParts[k];
            ImGui::PushID(4000 + static_cast<int>(k));
            const bool partSel = static_cast<int>(k) == m_selPart;
            char header[64];
            std::snprintf(header, sizeof(header), "part %d%s###parthdr", static_cast<int>(k),
                          partSel ? " (selected)" : "");
            if (ImGui::Selectable(header, partSel))
                m_selPart = static_cast<int>(k);
            ImGui::SameLine();
            if (ImGui::SmallButton("X##part"))
            {
                p.collideParts.erase(p.collideParts.begin() + static_cast<std::ptrdiff_t>(k));
                if (m_selPart >= static_cast<int>(p.collideParts.size()))
                    m_selPart = -1;
                MarkDirty();
                ImGui::PopID();
                break;
            }
            if (ImGui::DragFloat3("offset", part.off.data(), 0.1f, -60.0f, 60.0f, "%.2f"))
                MarkDirty();
            if (ImGui::DragFloat3("size", part.size.data(), 0.1f, 0.1f, 120.0f, "%.2f"))
                MarkDirty();
            if (ImGui::DragFloat("yawDeg", &part.yawDeg, 1.0f, -180.0f, 180.0f, "%.0f"))
                MarkDirty();
            ImGui::PopID();
        }
        if (p.collideParts.size() < kMaxCollideParts && ImGui::SmallButton("Add part"))
        {
            CollidePart part;
            part.size = {1.0f, 3.0f, 1.0f}; // pillar-ish default (full sizes)
            part.off = {0.0f, 1.5f, 0.0f};  // center sits half its height up
            p.collideParts.push_back(part);
            p.collide = true; // loader rule: parts imply collide
            m_selPart = static_cast<int>(p.collideParts.size()) - 1;
            MarkDirty();
        }

        ImGui::Spacing();
        if (ImGui::Button("Duplicate piece"))
        {
            Piece copy = p; // p may dangle after push_back - copy first
            copy.offX += 4.0f;
            copy.offZ += 4.0f;
            t.pieces.push_back(std::move(copy));
            SelectPiece(static_cast<int>(t.pieces.size()) - 1, -1);
            MarkDirty();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove piece"))
        {
            t.pieces.erase(t.pieces.begin() + static_cast<std::ptrdiff_t>(m_selPiece));
            SelectPiece(-1, -1);
            MarkDirty();
        }
    }

    void DecorLayoutEditorPanel::RenderValidationAndSave()
    {
        if (ImGui::Button("Validate"))
            RunValidation();
        ImGui::SameLine();
        const bool blocked = !m_validationRan || !m_violations.empty();
        if (ImGui::Button("Save decor.json"))
        {
            RunValidation();
            if (m_violations.empty() || m_overrideSave)
            {
                std::string err;
                if (SaveToDisk(err))
                {
                    m_statusMsg = "Saved. Backup written to decor.json.bak; ParseStrict re-read OK.";
                    m_statusIsError = false;
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "DecorLayoutEditorPanel: saved '%s'",
                                   m_dataPath.c_str());
                }
                else
                {
                    m_statusMsg = "Save FAILED: " + err;
                    m_statusIsError = true;
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor, "DecorLayoutEditorPanel: %s", m_statusMsg.c_str());
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
                ImGui::BeginChild("decor_violation_list", ImVec2(0.0f, 140.0f), true);
                for (const std::string& v : m_violations)
                    ImGui::TextWrapped("- %s", v.c_str());
                ImGui::EndChild();
                ImGui::Checkbox("I know what I am doing (save despite violations)", &m_overrideSave);
            }
        }
    }

} // namespace SparkEditor
