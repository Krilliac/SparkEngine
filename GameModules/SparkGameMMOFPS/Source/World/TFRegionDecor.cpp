/**
 * @file TFRegionDecor.cpp
 * @brief Per-tier region decor stamper (see TFRegionDecor.h).
 *
 * Follows TFRegionSystem::UpdateCaptureVisuals / TFVehicleTerminal to the
 * letter: spawn once when the ECS world + terrain + data tables are all live,
 * viewer-only (the owner gates on HasLocalPlayer), Transform+MeshRenderer
 * entities with NO physics bodies. decor.json parsing goes through
 * Spark::Json (JsonUtils.h) like TFDataTables.cpp, but stays local to this
 * file — the data-tables surface is contended and this table is decor-only.
 */
#include "World/TFRegionDecor.h"

#include "Data/TFDataTables.h"
#include "Game/TFVisualUtils.h" // FactionStructureMaterial (neutral/faction tint)
#include "World/TFWorldSetup.h" // TerrainHeightAt for per-piece placement

#include "Engine/ECS/Components.h" // Transform, MeshRenderer
#include "Spark/IEngineContext.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        constexpr const char* kDecorFile = "Assets/MMOFPS/Data/decor.json"; // TFDataTables kDataDir convention
        constexpr uint32_t kMaxDecorPerRegion = 20; // lane budget (cap tower/banner/terminals not counted here)
        constexpr float kDecorSeparationM = 5.0f;   // scatter props keep clear of placed decor
        constexpr int kScatterAttempts = 12;        // rejection-sampling tries per scatter prop
        constexpr float kTwoPi = 6.2831853f;

        /// The four regions.json tier strings (TFDataTables validates them).
        constexpr const char* kTiers[] = {"outpost", "fort", "facility", "skyanchor"};

        /// Tiny deterministic LCG (Numerical Recipes constants) so every client
        /// scatters identically from the same RegionId seed. NOT std::rand /
        /// std::mt19937: no cross-CRT/no-header-weight, and trivially stable.
        struct DecorRng
        {
            uint32_t s;
            explicit DecorRng(uint32_t seed) : s(seed ^ 0x9E3779B9u) {}
            uint32_t Next()
            {
                s = s * 1664525u + 1013904223u;
                return s;
            }
            float NextFloat01() { return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f); }
        };

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

    TFRegionDecor::TFRegionDecor() = default;
    TFRegionDecor::~TFRegionDecor()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFRegionDecor::Initialize(TFGameContext& ctx)
    {
        m_ctx = &ctx;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_decor_debug"))
        {
            console.RegisterCommand(
                "tf_decor_debug",
                [this](const std::vector<std::string>&) -> std::string
                {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                                  "[TF] decor: spawned=%d entities=%u skipped(clearance)=%u skipped(budget)=%u "
                                  "templates=%zu",
                                  m_spawned ? 1 : 0, SpawnedCount(), m_skippedClearance, m_skippedBudget,
                                  m_templates.size());
                    return std::string(buf);
                },
                "Region decor status: entity total, clearance/budget skips, loaded tier templates", "TERRAFRONT",
                "tf_decor_debug");
            m_debugCmd = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFRegionDecor initialized");
        return true;
    }

    void TFRegionDecor::Update()
    {
        if (!m_initialized || !m_ctx || m_spawned || !m_ctx->HasLocalPlayer())
            return;
        SpawnAll();
    }

    void TFRegionDecor::Shutdown()
    {
        if (!m_initialized)
            return;

        if (m_debugCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_decor_debug");
            m_debugCmd = false;
        }

        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (world)
        {
            for (const uint32_t ent : m_entities)
            {
                const auto e = static_cast<EntityID>(ent);
                if (ent != 0u && world->GetRegistry().valid(e))
                    world->DestroyEntity(e);
            }
        }
        m_entities.clear();
        m_templates.clear();
        m_spawned = false;
        m_loadTried = false;
        m_loaded = false;
        m_initialized = false;
    }

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

    // ---------------------------------------------------------------------------
    // Stamp
    // ---------------------------------------------------------------------------

    bool TFRegionDecor::ClearOfGameplay(const RegionDef& r, float x, float z) const
    {
        const float clearSq = m_clearanceM * m_clearanceM;
        const auto tooClose = [&](float px, float pz)
        {
            const float dx = x - px;
            const float dz = z - pz;
            return dx * dx + dz * dz < clearSq;
        };
        for (const auto& pt : r.capturePoints)
            if (tooClose(pt[0], pt[1]))
                return false;
        for (const auto& sp : r.spawns)
            if (tooClose(sp[0], sp[1]))
                return false;
        if (r.vehicleTerminal.has_value() && tooClose((*r.vehicleTerminal)[0], (*r.vehicleTerminal)[1]))
            return false;
        return true;
    }

    void TFRegionDecor::SpawnAll()
    {
        // Same readiness bar as the capture landmarks: ECS world + terrain + data.
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || !m_ctx->world || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const auto& regions = m_ctx->data->GetContinent().regions;
        if (regions.empty())
            return;
        if (!LoadTemplates())
        {
            m_spawned = true; // fail-soft: don't retry every frame, regions stay bare
            return;
        }

        uint32_t stampedRegions = 0;
        for (const RegionDef& r : regions)
        {
            const auto it = m_templates.find(r.tier);
            if (it == m_templates.end())
                continue;
            const TierTemplate& tmpl = it->second;

            // Skyanchors never flip, so their decor tints by home faction;
            // everything else stays neutral (no per-frame retint loop — decor
            // is fire-and-forget, unlike the owner-tinted banners/terminals).
            const FactionId tint = (r.tier == "skyanchor") ? r.homeFaction : FactionId::None;
            const float centerY = m_ctx->world->TerrainHeightAt(r.centerX, r.centerZ);

            uint32_t placed = 0;
            std::vector<std::array<float, 2>> placedXZ; // scatter separation targets
            placedXZ.reserve(kMaxDecorPerRegion);

            const auto spawnPiece = [&](const std::string& model, const std::string& material, float x, float z,
                                        float yawDeg, bool terrainAlign, bool castShadows, float emissive) -> bool
            {
                if (placed >= kMaxDecorPerRegion)
                {
                    ++m_skippedBudget;
                    return false;
                }
                if (!ClearOfGameplay(r, x, z))
                {
                    ++m_skippedClearance;
                    return false;
                }
                const float y = terrainAlign ? m_ctx->world->TerrainHeightAt(x, z) : centerY;
                const auto ent = world->CreateEntity("TF_Decor");
                Transform& tr = world->AddComponent<Transform>(ent);
                tr.position = {x, y, z};
                tr.rotation.y = yawDeg; // Transform Euler is DEGREES (radians rule is PhysicsBody-only)
                MeshRenderer& mr = world->AddComponent<MeshRenderer>(ent);
                mr.meshPath = model;
                mr.materialPath = material.empty() ? FactionStructureMaterial(*m_ctx, tint) : material;
                mr.castShadows = castShadows;
                mr.emissive = emissive;
                m_entities.push_back(static_cast<uint32_t>(ent));
                placedXZ.push_back({x, z});
                ++placed;
                return true;
            };

            // 1) Fixed template pieces — identical layout on every client.
            for (const DecorPiece& p : tmpl.pieces)
                spawnPiece(p.model, p.material, r.centerX + p.offX, r.centerZ + p.offZ, p.yawDeg, p.terrainAlign,
                           p.castShadows, p.emissive);

            // 2) Seeded prop scatter — RegionId-seeded so every client rolls the
            //    same props at the same spots (rejection sampling is fine: same
            //    code + same data => same accept/reject sequence everywhere).
            const ScatterSpec& sc = tmpl.scatter;
            if (!sc.models.empty() && sc.countMax > 0 && sc.radiusMax > 0.0f)
            {
                DecorRng rng(static_cast<uint32_t>(r.id) * 2654435761u + 1u);
                const int want =
                    sc.countMin + static_cast<int>(rng.Next() % static_cast<uint32_t>(sc.countMax - sc.countMin + 1));
                for (int n = 0; n < want; ++n)
                {
                    for (int attempt = 0; attempt < kScatterAttempts; ++attempt)
                    {
                        const float ang = rng.NextFloat01() * kTwoPi;
                        const float rad = sc.radiusMin + rng.NextFloat01() * (sc.radiusMax - sc.radiusMin);
                        const float x = r.centerX + std::sin(ang) * rad;
                        const float z = r.centerZ + std::cos(ang) * rad;
                        const std::string& model = sc.models[rng.Next() % sc.models.size()];
                        const float yaw = rng.NextFloat01() * 360.0f;

                        // Keep clutter out of the stamped buildings' footprints.
                        bool nearDecor = false;
                        for (const auto& q : placedXZ)
                        {
                            const float dx = x - q[0];
                            const float dz = z - q[1];
                            if (dx * dx + dz * dz < kDecorSeparationM * kDecorSeparationM)
                            {
                                nearDecor = true;
                                break;
                            }
                        }
                        if (nearDecor || !ClearOfGameplay(r, x, z))
                            continue;
                        spawnPiece(model, std::string(), x, z, yaw, true, true, 0.0f);
                        break;
                    }
                }
            }

            if (placed > 0)
                ++stampedRegions;
        }

        m_spawned = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] decor: stamped %u regions, %zu entities (skipped %u clearance, %u budget)", stampedRegions,
                       m_entities.size(), m_skippedClearance, m_skippedBudget);
    }

} // namespace Terrafront
