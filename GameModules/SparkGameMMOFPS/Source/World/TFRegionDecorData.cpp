/**
 * @file TFRegionDecorData.cpp
 * @brief TFRegionDecor decor.json parsing (LoadTemplates): tier templates,
 *        pieces, collideParts and scatter specs. Split from TFRegionDecor.cpp
 *        per the repo file-size rules (same class — mirrors the TFWorldSetup/
 *        -Draw/-Render split); lifecycle stays in TFRegionDecor.cpp, layout in
 *        TFRegionDecorLayout.cpp.
 *
 * decor.json parsing goes through Spark::Json (JsonUtils.h) like
 * TFDataTables.cpp, but stays local to this file — the data-tables surface is
 * contended and this table is decor-only.
 */
#include "World/TFRegionDecor.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        constexpr const char* kDecorFile = "Assets/MMOFPS/Data/decor.json"; // TFDataTables kDataDir convention
        constexpr size_t kMaxCollideParts = 5; // W11 lane budget: keep per-piece part counts small

        /// The four regions.json tier strings (TFDataTables validates them).
        constexpr const char* kTiers[] = {"outpost", "fort", "facility", "skyanchor"};

        using Spark::Json::Value;

        float GetNum(const Value& o, const char* k, float def)
        {
            return o.HasKey(k) ? static_cast<float>(o[k].AsNumber(def)) : def;
        }

        bool GetBool(const Value& o, const char* k, bool def)
        {
            return o.HasKey(k) ? o[k].AsBool(def) : def;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // decor.json -> tier templates
    // ---------------------------------------------------------------------------

    bool TFRegionDecor::LoadTemplates()
    {
        if (m_loadTried)
            return m_loaded;
        m_loadTried = true;

        std::ifstream f(kDecorFile, std::ios::binary);
        if (!f.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] decor: cannot open '%s' — regions stay bare", kDecorFile);
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        const Value root = Spark::Json::Parse(ss.str());
        if (!root.IsObject() || !root.HasKey("templates") || !root["templates"].IsObject())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] decor: '%s' malformed (need object with 'templates')",
                            kDecorFile);
            return false;
        }

        m_clearanceM = GetNum(root, "clearanceM", 8.0f);

        const Value& templates = root["templates"];
        for (const char* tier : kTiers)
        {
            if (!templates.HasKey(tier) || !templates[tier].IsObject())
                continue;
            const Value& t = templates[tier];
            TierTemplate out;

            if (t.HasKey("pieces") && t["pieces"].IsArray())
            {
                const Value& pieces = t["pieces"];
                for (size_t i = 0; i < pieces.Size(); ++i)
                {
                    const Value& p = pieces[i];
                    if (!p.IsObject() || !p.HasKey("model") || !p.HasKey("offset") || !p["offset"].IsArray() ||
                        p["offset"].Size() < 2)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] decor: %s piece %zu missing model/offset[2]",
                                       tier, i);
                        continue;
                    }
                    DecorPiece piece;
                    piece.model = p["model"].AsString(std::string());
                    piece.material = p.HasKey("material") ? p["material"].AsString(std::string()) : std::string();
                    piece.offX = static_cast<float>(p["offset"][0].AsNumber(0.0));
                    piece.offZ = static_cast<float>(p["offset"][1].AsNumber(0.0));
                    piece.yawDeg = GetNum(p, "yaw", 0.0f);
                    piece.terrainAlign = GetBool(p, "terrainAlign", true);
                    piece.castShadows = GetBool(p, "castShadows", true);
                    piece.emissive = GetNum(p, "emissive", 0.0f);
                    // W10: opt-IN collision (buildings). Absent flag = visual-only,
                    // so a new decor.json entry can never surprise movement.
                    piece.collide = GetBool(p, "collide", false);
                    // W11 gate-passages: optional "collideParts" — model-local
                    // boxes ({offset:[x,y,z], size:[x,y,z], yawDeg}) that
                    // REPLACE the whole-model OBB so archways stay open.
                    // Authoring parts implies collision (a part list on a
                    // visual-only piece would be meaningless dead data).
                    if (p.HasKey("collideParts") && p["collideParts"].IsArray())
                    {
                        const Value& parts = p["collideParts"];
                        for (size_t k = 0; k < parts.Size(); ++k)
                        {
                            const Value& pv = parts[k];
                            if (!pv.IsObject() || !pv.HasKey("offset") || !pv["offset"].IsArray() ||
                                pv["offset"].Size() < 3 || !pv.HasKey("size") || !pv["size"].IsArray() ||
                                pv["size"].Size() < 3)
                            {
                                SPARK_LOG_WARN(Spark::LogCategory::Game,
                                               "[TF] decor: %s piece %zu collidePart %zu missing offset[3]/size[3]",
                                               tier, i, k);
                                continue;
                            }
                            if (piece.collideParts.size() >= kMaxCollideParts)
                            {
                                SPARK_LOG_WARN(Spark::LogCategory::Game,
                                               "[TF] decor: %s piece %zu exceeds %zu collideParts - extras dropped",
                                               tier, i, kMaxCollideParts);
                                break;
                            }
                            DecorCollidePart part;
                            for (int a = 0; a < 3; ++a)
                            {
                                part.off[a] = static_cast<float>(pv["offset"][a].AsNumber(0.0));
                                part.size[a] = static_cast<float>(pv["size"][a].AsNumber(1.0));
                            }
                            part.yawDeg = GetNum(pv, "yawDeg", 0.0f);
                            piece.collideParts.push_back(part);
                        }
                        if (!piece.collideParts.empty())
                            piece.collide = true;
                    }
                    if (!piece.model.empty())
                        out.pieces.push_back(std::move(piece));
                }
            }

            if (t.HasKey("scatter") && t["scatter"].IsObject())
            {
                const Value& s = t["scatter"];
                ScatterSpec& sc = out.scatter;
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
                    sc.radiusMax = static_cast<float>(s["radius"][1].AsNumber(sc.radiusMin));
                }
                if (sc.countMax < sc.countMin)
                    sc.countMax = sc.countMin;
                if (sc.radiusMax < sc.radiusMin)
                    sc.radiusMax = sc.radiusMin;
            }

            m_templates.emplace(tier, std::move(out));
        }

        m_loaded = !m_templates.empty();
        if (!m_loaded)
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] decor: '%s' defined no usable tier templates", kDecorFile);
        return m_loaded;
    }

} // namespace Terrafront
