/**
 * @file TFRegionDecor.cpp
 * @brief Role-agnostic region decor layout + visual stamp + static collision
 *        (see TFRegionDecor.h for the W10 split and the determinism contract).
 *
 * Layout follows the TFRegionSystem::UpdateCaptureVisuals readiness bar minus
 * the ECS world (dedicated servers may run without one — the 2026-07-10
 * headless-boot lesson); visuals additionally wait for the ECS world and
 * HasLocalPlayer. decor.json parsing goes through Spark::Json (JsonUtils.h)
 * like TFDataTables.cpp, but stays local to this file — the data-tables
 * surface is contended and this table is decor-only.
 */
#include "World/TFRegionDecor.h"

#include "Data/TFDataTables.h"
#include "Game/TFVisualUtils.h"     // FactionStructureMaterial (neutral/faction tint)
#include "World/TFWorldCollision.h" // AddModelObb / RemoveBody / OptimizeBroadPhase
#include "World/TFWorldSetup.h"     // TerrainHeightAt + Collision() accessor

#include "Camera/SparkEngineCamera.h" // W10 distance culling: camera position
#include "Engine/ECS/Components.h"    // Transform, MeshRenderer
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
        constexpr float kDegToRad = 0.01745329252f; // W11: part footprints (same constant as TFWorldCollision)
        constexpr size_t kMaxCollideParts = 5;      // W11 lane budget: keep per-piece part counts small

        /// The four regions.json tier strings (TFDataTables validates them).
        constexpr const char* kTiers[] = {"outpost", "fort", "facility", "skyanchor"};

        /// Tiny deterministic LCG (Numerical Recipes constants) so every role
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

        /// W10 distance-culling lane: cull class from the asset convention —
        /// scatter clutter lives under Models/MMOFPS/props/, kit pieces under
        /// buildings/. Anything unrecognized defaults to the building range
        /// (larger = safer visually; worst case it just culls later).
        float DecorCullRangeForModel(const std::string& model)
        {
            return model.find("/props/") != std::string::npos ? kDecorCullRangePropM : kDecorCullRangeBuildingM;
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
                    char buf[384];
                    std::snprintf(buf, sizeof(buf),
                                  "[TF] decor: layout=%d pieces=%zu visuals=%u colBodies=%u colSkipped=%u "
                                  "colDemoted=%u skipped(clearance)=%u skipped(budget)=%u templates=%zu "
                                  "cullVisible=%u cullHidden=%u",
                                  m_layoutDone ? 1 : 0, m_layout.size(), SpawnedCount(), CollisionBodyCount(),
                                  m_colSkipped, m_colDemoted, m_skippedClearance, m_skippedBudget, m_templates.size(),
                                  VisibleDecorCount(), CulledDecorCount());
                    return std::string(buf);
                },
                "Region decor status: layout pieces, visual entities, collision bodies, clearance/budget skips, "
                "distance-cull visible/hidden counts",
                "TERRAFRONT", "tf_decor_debug");
            m_debugCmd = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFRegionDecor initialized");
        return true;
    }

    void TFRegionDecor::Update()
    {
        if (!m_initialized || !m_ctx)
            return;
        if (!m_layoutDone && !TryComputeLayout())
            return; // prerequisites not live yet — poll again next frame
        if (!m_collisionDone)
            RegisterCollision();
        if (!m_visualsDone && m_ctx->HasLocalPlayer())
            SpawnVisuals();
        if (m_visualsDone && !m_cull.empty())
            UpdateCulling(); // W10 distance-culling lane: ~4 Hz visibility pass
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
        m_cull.clear(); // W10 distance-culling lane
        m_cullFrameCounter = 0;
        m_cullVisible = 0;
        m_cullHidden = 0;

        // Collision handles: Main.cpp shuts TFRegionSystem (and therefore us)
        // down BEFORE TFWorldSetup, so the scene collision set is still alive
        // here; if it is already gone, RemoveBody on a foreign handle is a
        // safe no-op and the set's own Shutdown removed the bodies anyway.
        TFWorldCollision* col = (m_ctx && m_ctx->world) ? m_ctx->world->Collision() : nullptr;
        if (col)
        {
            for (const auto& body : m_colBodies)
                col->RemoveBody(body);
        }
        m_colBodies.clear();

        m_layout.clear();
        m_templates.clear();
        m_layoutDone = false;
        m_collisionDone = false;
        m_visualsDone = false;
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

    // ---------------------------------------------------------------------------
    // Role-agnostic layout (W10) — see the determinism contract in the header
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

    bool TFRegionDecor::TryComputeLayout()
    {
        // Readiness bar: data tables + analytic terrain. Deliberately NOT the
        // ECS world — dedicated servers may run headless without one, and the
        // layout (and its collision) must exist there regardless.
        if (!m_ctx->world || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false;
        const auto& regions = m_ctx->data->GetContinent().regions;
        if (regions.empty())
            return false;
        if (!LoadTemplates())
        {
            m_layoutDone = true; // fail-soft: don't retry every frame, regions stay bare
            return true;
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

            const auto addPiece = [&](const std::string& model, const std::string& material, float x, float z,
                                      float yawDeg, bool terrainAlign, bool castShadows, float emissive, bool collide,
                                      const std::vector<DecorCollidePart>* collideParts) -> bool
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
                LayoutPiece piece;
                piece.model = model;
                piece.material = material;
                piece.tint = tint;
                piece.region = r.id;
                piece.x = x;
                piece.y = terrainAlign ? m_ctx->world->TerrainHeightAt(x, z) : centerY;
                piece.z = z;
                piece.yawDeg = yawDeg;
                piece.castShadows = castShadows;
                piece.emissive = emissive;
                piece.collide = collide;
                if (collideParts) // W11: multi-part collision shape rides the layout
                    piece.collideParts = *collideParts;
                m_layout.push_back(std::move(piece));
                placedXZ.push_back({x, z});
                ++placed;
                return true;
            };

            // 1) Fixed template pieces — identical layout on every role.
            for (const DecorPiece& p : tmpl.pieces)
                addPiece(p.model, p.material, r.centerX + p.offX, r.centerZ + p.offZ, p.yawDeg, p.terrainAlign,
                         p.castShadows, p.emissive, p.collide, &p.collideParts);

            // 2) Seeded prop scatter — RegionId-seeded so every role rolls the
            //    same props at the same spots (rejection sampling is fine: same
            //    code + same data => same accept/reject sequence everywhere).
            //    Scatter props NEVER collide (small clutter stays walk-through).
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
                        addPiece(model, std::string(), x, z, yaw, true, true, 0.0f, false, nullptr);
                        break;
                    }
                }
            }

            if (placed > 0)
                ++stampedRegions;
        }

        EnforceCollisionClearance();

        m_layoutDone = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] decor: layout %u regions, %zu pieces (skipped %u clearance, %u budget; %u demoted)",
                       stampedRegions, m_layout.size(), m_skippedClearance, m_skippedBudget, m_colDemoted);
        return true;
    }

    void TFRegionDecor::EnforceCollisionClearance()
    {
        // W10 sanity gate, enforced AT GENERATION: no collidable decor may sit
        // within m_clearanceM of ANY region's capture points/spawns/terminals —
        // ClearOfGameplay above already guaranteed the piece's OWN region, this
        // closes the cross-region edge (adjacent gameplay points near a shared
        // border). Violators are demoted to visual-only, deterministically on
        // both roles (pure function of the same layout + region table), loudly:
        // a demotion means decor.json offsets need re-authoring.
        const auto& regions = m_ctx->data->GetContinent().regions;
        for (LayoutPiece& p : m_layout)
        {
            if (!p.collide)
                continue;

            // W11 gate-passages: probe the piece origin AND every collide-part
            // center (parts sit off-origin — a gate pillar 2.6 m out can crowd
            // a gameplay point the origin check would miss). Part world XZ =
            // piece XZ + Ry(pieceYaw) * part offset — the same yaw mapping as
            // TFWorldCollision (x' = x*c + z*s, z' = -x*s + z*c). A violation
            // by ANY probe demotes the WHOLE piece (partial gates would look
            // arbitrary and complicate determinism reasoning for no benefit).
            std::vector<std::array<float, 2>> probes;
            probes.reserve(1 + p.collideParts.size());
            probes.push_back({p.x, p.z});
            const float ryRad = p.yawDeg * kDegToRad;
            const float cyaw = std::cos(ryRad), syaw = std::sin(ryRad);
            for (const DecorCollidePart& part : p.collideParts)
                probes.push_back(
                    {part.off[0] * cyaw + part.off[2] * syaw + p.x, -part.off[0] * syaw + part.off[2] * cyaw + p.z});

            bool demoted = false;
            for (const RegionDef& r : regions)
            {
                for (const auto& q : probes)
                {
                    if (ClearOfGameplay(r, q[0], q[1]))
                        continue;
                    SPARK_LOG_ERROR(
                        Spark::LogCategory::Game,
                        "[TF] decor: CLEARANCE VIOLATION — collidable '%s' (region %u) within %.1fm of region "
                        "%u gameplay point; demoted to visual-only (fix decor.json offsets)",
                        p.model.c_str(), static_cast<unsigned>(p.region), static_cast<double>(m_clearanceM),
                        static_cast<unsigned>(r.id));
                    p.collide = false;
                    ++m_colDemoted;
                    demoted = true;
                    break;
                }
                if (demoted)
                    break;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Collision pass (both roles)
    // ---------------------------------------------------------------------------

    void TFRegionDecor::RegisterCollision()
    {
        // The scene TFWorldCollision was built inside TFWorldSetup::Initialize —
        // strictly before the first TFRegionSystem::Update (Main.cpp boot order)
        // — so registering here is always AFTER the .scene static bodies.
        TFWorldCollision* col = m_ctx->world ? m_ctx->world->Collision() : nullptr;
        if (!col)
        {
            m_collisionDone = true; // no collision owner shipped: decor stays walk-through
            return;
        }

        uint32_t added = 0;
        for (const LayoutPiece& p : m_layout)
        {
            if (!p.collide)
                continue;
            const float pos[3] = {p.x, p.y, p.z};

            // W11 gate-passages: authored parts REPLACE the whole-model OBB —
            // one rotated box per part (pillars/legs/lintels), leaving the
            // archway/underside genuinely walkable. Parts inherit the piece's
            // determinism (pure data + the bit-identical layout) and its
            // clearance demotion (p.collide was cleared above if any part
            // footprint violated). m_colSkipped counts per BODY, so a partly
            // failed piece still reports every miss.
            if (!p.collideParts.empty())
            {
                for (size_t k = 0; k < p.collideParts.size(); ++k)
                {
                    const DecorCollidePart& part = p.collideParts[k];
                    const std::string name = "TF_DecorCol_r" + std::to_string(p.region) + "_" +
                                             std::to_string(static_cast<unsigned>(m_colBodies.size())) + "_p" +
                                             std::to_string(static_cast<unsigned>(k));
                    std::shared_ptr<::PhysicsBody> body =
                        col->AddObbPart(pos, p.yawDeg, part.off, part.size, part.yawDeg, name);
                    if (body)
                    {
                        m_colBodies.push_back(std::move(body));
                        ++added;
                    }
                    else
                    {
                        ++m_colSkipped; // no Jolt / body cap — logged inside
                    }
                }
                continue;
            }

            const std::string name = "TF_DecorCol_r" + std::to_string(p.region) + "_" +
                                     std::to_string(static_cast<unsigned>(m_colBodies.size()));
            std::shared_ptr<::PhysicsBody> body = col->AddModelObb(p.model, pos, p.yawDeg, name);
            if (body)
            {
                m_colBodies.push_back(std::move(body));
                ++added;
            }
            else
            {
                ++m_colSkipped; // no Jolt / unreadable OBJ / body cap — logged inside
            }
        }

        // ONE broadphase re-compact after ALL decor statics (the scene set was
        // compacted once in Build(); this is the second and last static bulk).
        if (added > 0)
            col->OptimizeBroadPhase();

        m_collisionDone = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] decor: %u static OBBs registered (%u skipped, %u demoted)",
                       added, m_colSkipped, m_colDemoted);
    }

    // ---------------------------------------------------------------------------
    // Visual pass (HasLocalPlayer roles only)
    // ---------------------------------------------------------------------------

    void TFRegionDecor::SpawnVisuals()
    {
        // Visuals additionally need the engine ECS world (poll until it exists;
        // headless roles never reach here — Update gates on HasLocalPlayer).
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return;

        for (const LayoutPiece& p : m_layout)
        {
            const auto ent = world->CreateEntity("TF_Decor");
            Transform& tr = world->AddComponent<Transform>(ent);
            tr.position = {p.x, p.y, p.z};
            tr.rotation.y = p.yawDeg; // Transform Euler is DEGREES (radians rule is PhysicsBody-only)
            MeshRenderer& mr = world->AddComponent<MeshRenderer>(ent);
            mr.meshPath = p.model;
            mr.materialPath = p.material.empty() ? FactionStructureMaterial(*m_ctx, p.tint) : p.material;
            mr.castShadows = p.castShadows;
            mr.emissive = p.emissive;
            m_entities.push_back(static_cast<uint32_t>(ent));
            // W10 distance-culling lane: record spawn-time XZ + cull class
            // (decor never moves, so the entry never needs updating).
            m_cull.push_back({static_cast<uint32_t>(ent), p.x, p.z, DecorCullRangeForModel(p.model), /*visible*/ true});
        }

        m_visualsDone = true;
        m_cullVisible = static_cast<uint32_t>(m_cull.size()); // until the first cull pass runs
        m_cullHidden = 0;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] decor: %zu visual entities stamped", m_entities.size());
    }

    // ---------------------------------------------------------------------------
    // Distance culling (W10 distance-culling lane — SEPARATE section by design;
    // see World/TFDecorCulling.h for ranges, hysteresis and the rationale)
    // ---------------------------------------------------------------------------

    void TFRegionDecor::UpdateCulling()
    {
        // Re-evaluate every kDecorCullIntervalFrames frames (~0.25 s @ 60 fps);
        // the first call after SpawnVisuals (counter == 0) runs immediately so
        // far decor never draws even one full-rate frame burst.
        if (m_cullFrameCounter++ % kDecorCullIntervalFrames != 0)
            return;

        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (!world || !cam)
            return;
        const auto camPos = cam->GetPosition(); // XZ only: decor culls by ground distance

        uint32_t visible = 0;
        uint32_t hidden = 0;
        auto& registry = world->GetRegistry();
        for (TFDecorCullEntry& e : m_cull)
        {
            const auto ent = static_cast<EntityID>(e.entity);
            if (e.entity == 0u || !registry.valid(ent))
                continue;
            const float dx = e.x - camPos.x;
            const float dz = e.z - camPos.z;
            const bool want = DecorShouldBeVisible(dx * dx + dz * dz, e.cullRange, e.visible);
            if (want != e.visible)
            {
                // MeshRenderer.visible short-circuits TFWorldSetup's ECS draw
                // loop before any mesh load or draw call — this flag IS the cull.
                if (MeshRenderer* mr = registry.try_get<MeshRenderer>(ent))
                    mr->visible = want;
                e.visible = want;
            }
            if (want)
                ++visible;
            else
                ++hidden;
        }
        m_cullVisible = visible;
        m_cullHidden = hidden;
    }

} // namespace Terrafront
