/**
 * @file TFWorldSetup.cpp
 * @brief Scene/terrain load, WorldServer/AreaServer boot, origin-rebase driving.
 *
 * Terrain model: procedural heightfield — dune base + a canyon separating the
 * SW/SE quadrants + one flat plateau per region (from regions.json). Every
 * object in cindral_wastes.scene is authored at its region's plateau height,
 * so TerrainHeightAt() and the scene agree exactly at every build site.
 *
 * Split per the repo file-size rules (mirrors the TFRegionSystem/-Net split):
 * this file owns lifecycle + scene/terrain/collision; draw helpers live in
 * TFWorldSetupDraw.cpp, frame composition in TFWorldSetupRender.cpp, network
 * boot in TFWorldSetupNet.cpp, frame driving in TFWorldSetupUpdate.cpp.
 */
#include "World/TFWorldSetup.h"

#include "World/TFSanctuaryZone.h"
#include "World/TFWorldCollision.h"

#include "Data/TFDataTables.h"

#include "Spark/IEngineContext.h"
#include "SceneManager/SceneManager.h"
#include "Engine/World/WorldOriginSystem.h"
#include "Camera/SparkEngineCamera.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/WorldServer.h"
#include "Engine/Networking/AreaServer.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

namespace Terrafront
{

    namespace
    {

        constexpr const char* kFallbackScenePath = "Assets/Scenes/MMOFPS/cindral_wastes.scene";
        // Continents lane: the additive sanctuary zone (see TFSanctuaryZone.h).
        constexpr const char* kSanctuaryScenePath = "Assets/Scenes/MMOFPS/sanctuary_haven.scene";
        constexpr float kOriginRebaseThreshold = 8192.0f; // > continent diagonal: mechanism wired but
                                                          // inert on the 4km map. TF-W2: lower once
                                                          // replication is origin-offset aware.

        float SmoothStep(float e0, float e1, float x)
        {
            if (e1 <= e0)
                return x < e0 ? 0.0f : 1.0f;
            float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

    } // namespace

    TFWorldSetup::TFWorldSetup() = default;
    TFWorldSetup::~TFWorldSetup()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFWorldSetup::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        m_origin = std::make_unique<Spark::World::WorldOriginSystem>();
        m_origin->SetRebasingThreshold(kOriginRebaseThreshold);
        m_origin->SetEnabled(true);

        LoadSceneAndTerrain();
        LoadSanctuaryScene();
        CreateCamera();

        // Static Jolt collision from the authored scene — built on BOTH server and
        // client (deterministic: parsed straight from the same .scene file, so it
        // works on headless dedicated servers that have no SceneManager). Terrain
        // stays analytic (TerrainHeightAt); see TFWorldCollision.h for the split.
        m_collision = std::make_unique<TFWorldCollision>();
        m_collision->Build(ctx, m_scenePath);

        // Sanctuary Haven static bodies (continents lane): a second, independent
        // TFWorldCollision over the additive zone scene. Same determinism
        // contract (parsed from the same file on both roles); inactive when the
        // file is missing or Jolt is absent.
        m_sanctuaryCollision = std::make_unique<TFWorldCollision>();
        m_sanctuaryCollision->Build(ctx, kSanctuaryScenePath);

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFWorldSetup initialized (scene=%s, loaded=%s)",
                       m_scenePath.c_str(), m_sceneLoaded ? "yes" : "no");
        return true;
    }

    void TFWorldSetup::LoadSceneAndTerrain()
    {
        if (m_ctx->data && m_ctx->data->IsLoaded() && !m_ctx->data->GetContinent().scene.empty())
            m_scenePath = "Assets/" + m_ctx->data->GetContinent().scene;
        else
            m_scenePath = kFallbackScenePath;

        // Heightfield params live in the scene's [Terrain] section (tf* keys) so
        // authored geometry and the runtime height function share one source.
        ParseTerrainParams(m_scenePath);

        // The engine registers NO SceneManager in module mode (the FPS module
        // builds its own too — see SparkGameFPS Game::Initialize). Reuse the
        // engine's if one ever appears; otherwise own one, exactly like the FPS
        // module does. Requires graphics (skipped headless).
        SceneManager* sm = m_ctx->engine ? m_ctx->engine->GetSceneManager() : nullptr;
        if (!sm && m_ctx->engine && m_ctx->engine->GetGraphics())
        {
            m_ownScene = std::make_unique<SceneManager>(m_ctx->engine->GetGraphics(), m_ctx->engine->GetInput());
            sm = m_ownScene.get();
        }
        m_scene = sm;
        if (sm)
        {
            std::wstring wpath(m_scenePath.begin(), m_scenePath.end());
            m_sceneLoaded = sm->LoadScene(wpath);
            Spark::SimpleConsole::GetInstance().LogInfo(std::string("[TF] scene ") + m_scenePath +
                                                        (m_sceneLoaded ? " loaded (" : " FAILED (") +
                                                        std::to_string(sm->GetObjects().size()) + " objects)");
            if (!m_sceneLoaded)
                SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] Scene load failed: %s (headless or missing file?)",
                               m_scenePath.c_str());
        }
        else
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game,
                           "[TF] No SceneManager (headless server) - skipping visual scene load");
        }
        // TF-W2: feed the heightfield into ClipmapTerrain/TerrainRenderer so the
        // visual terrain matches TerrainHeightAt instead of the flat ground plane.
    }

    void TFWorldSetup::LoadSanctuaryScene()
    {
        // Continents lane: sanctuary_haven.scene is an ADDITIVE zone of the same
        // world. SceneManager::LoadScene clears its node set, so the zone gets
        // its OWN manager; RenderWorld draws both through DrawSceneObjects.
        // Requires graphics (dedicated servers skip visuals; their collision
        // comes from m_sanctuaryCollision, parsed straight from the file).
        GraphicsEngine* gfx = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetGraphics() : nullptr;
        if (!gfx)
            return;
        m_sanctuaryScene = std::make_unique<SceneManager>(gfx, m_ctx->engine->GetInput());
        const std::string path = kSanctuaryScenePath;
        m_sanctuaryLoaded = m_sanctuaryScene->LoadScene(std::wstring(path.begin(), path.end()));
        Spark::SimpleConsole::GetInstance().LogInfo(
            std::string("[TF] scene ") + path + (m_sanctuaryLoaded ? " loaded (" : " FAILED (") +
            std::to_string(m_sanctuaryScene->GetObjects().size()) + " objects)");
        if (!m_sanctuaryLoaded)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] Sanctuary scene load failed: %s", path.c_str());
            m_sanctuaryScene.reset();
        }
    }

    void TFWorldSetup::ParseTerrainParams(const std::string& scenePath)
    {
        std::ifstream f(scenePath);
        if (!f.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] Cannot read %s for terrain params; using defaults",
                           scenePath.c_str());
            return;
        }

        std::string line;
        bool inTerrain = false;
        while (std::getline(f, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                inTerrain = (line == "[Terrain]");
                continue;
            }
            if (!inTerrain)
                continue;
            auto eq = line.find('=');
            if (eq == std::string::npos || line.rfind("tf", 0) != 0)
                continue;
            const std::string key = line.substr(0, eq);
            const float val = std::strtof(line.c_str() + eq + 1, nullptr);

            if (key == "tfBaseHeight")
                m_terrain.baseHeight = val;
            else if (key == "tfDuneAmp")
                m_terrain.duneAmp = val;
            else if (key == "tfDunePeriodX")
                m_terrain.dunePeriodX = val;
            else if (key == "tfDunePeriodZ")
                m_terrain.dunePeriodZ = val;
            else if (key == "tfRidgeAmp")
                m_terrain.ridgeAmp = val;
            else if (key == "tfCanyonX")
                m_terrain.canyonX = val;
            else if (key == "tfCanyonHalfWidth")
                m_terrain.canyonHalfW = val;
            else if (key == "tfCanyonZ0")
                m_terrain.canyonZ0 = val;
            else if (key == "tfCanyonZ1")
                m_terrain.canyonZ1 = val;
            else if (key == "tfCanyonDepth")
                m_terrain.canyonDepth = val;
            else if (key == "tfPlateauRadius")
                m_terrain.plateauRadius = val;
            else if (key == "tfPlateauSkirt")
                m_terrain.plateauSkirt = val;
            else if (key == "tfPlateauSkyanchor")
                m_terrain.plateauSky = val;
            else if (key == "tfPlateauFort")
                m_terrain.plateauFort = val;
            else if (key == "tfPlateauFacility")
                m_terrain.plateauFacility = val;
            else if (key == "tfPlateauOutpost")
                m_terrain.plateauOutpost = val;
        }
    }

    const WorldPresentationDef& TFWorldSetup::Pres() const
    {
        static const WorldPresentationDef kDefault{};
        return (m_ctx && m_ctx->data && m_ctx->data->IsLoaded()) ? m_ctx->data->GetPresentation() : kDefault;
    }

    float TFWorldSetup::PlateauHeight(const std::string& tier) const
    {
        if (tier == "skyanchor")
            return m_terrain.plateauSky;
        if (tier == "fort")
            return m_terrain.plateauFort;
        if (tier == "facility")
            return m_terrain.plateauFacility;
        return m_terrain.plateauOutpost;
    }

    float TFWorldSetup::TerrainHeightAt(float x, float z) const
    {
        const TFTerrainParams& p = m_terrain;

        // Dune base
        float h = p.baseHeight + p.duneAmp * std::sin(x * p.dunePeriodX) * std::cos(z * p.dunePeriodZ) +
                  p.ridgeAmp * std::sin(x * 0.013f + z * 0.011f);

        // Canyon between SW (AUC) and SE (HLX) territory
        const float nx = (x - p.canyonX) / p.canyonHalfW;
        if (std::fabs(nx) < 1.0f)
        {
            const float across = 1.0f - nx * nx;
            const float along = SmoothStep(p.canyonZ0 - 300.0f, p.canyonZ0, z) *
                                (1.0f - SmoothStep(p.canyonZ1, p.canyonZ1 + 300.0f, z));
            h -= p.canyonDepth * across * along;
        }

        // Flat mesa plateau around each region center (scene objects sit at
        // exactly these heights). Regions are far apart relative to the skirt,
        // so sequential blending is order-independent in practice.
        if (m_ctx && m_ctx->data && m_ctx->data->IsLoaded())
        {
            for (const RegionDef& r : m_ctx->data->GetContinent().regions)
            {
                const float dx = x - r.centerX;
                const float dz = z - r.centerZ;
                const float distSq = dx * dx + dz * dz;
                const float outer = p.plateauRadius + p.plateauSkirt;
                if (distSq >= outer * outer)
                    continue;
                const float w = 1.0f - SmoothStep(p.plateauRadius, outer, std::sqrt(distSq));
                h += (PlateauHeight(r.tier) - h) * w;
            }
        }

        // Sanctuary Haven pad (continents lane): a flat plateau at the reserved
        // NW-corner zone, blended exactly like the region plateaus above. The
        // constants are compile-time (TFSanctuaryZone.h) so this term is
        // identical on every role regardless of data-load state — the same
        // determinism contract as the rest of this function.
        {
            const float dx = x - kTFSanctuaryCenterX;
            const float dz = z - kTFSanctuaryCenterZ;
            const float distSq = dx * dx + dz * dz;
            const float outer = kTFSanctuaryPlateauRadius + kTFSanctuaryPlateauSkirt;
            if (distSq < outer * outer)
            {
                const float w = 1.0f - SmoothStep(kTFSanctuaryPlateauRadius, outer, std::sqrt(distSq));
                h += (kTFSanctuaryPadY - h) * w;
            }
        }
        return h;
    }

    void TFWorldSetup::ResolveMoveCollision(const float prevPos[3], float pos[3], float vel[3], bool* grounded) const
    {
        // 1) Capsule-sweep + slide against the static scene bodies (no-op when
        //    Jolt is absent or the scene produced no bodies). The sanctuary zone
        //    bodies are a second independent set (continents lane) — the sets are
        //    ~1.7 km apart, so resolving them sequentially never double-slides.
        if (m_collision)
            m_collision->ResolveMove(prevPos, pos, vel, grounded);
        if (m_sanctuaryCollision)
            m_sanctuaryCollision->ResolveMove(prevPos, pos, vel, grounded);

        // 2) Terrain backstop at the RESOLVED column: the slide can change the
        //    final XZ after TFMoveStep's own step-7 clamp ran, and this also keeps
        //    "never below terrain" true even with physics unavailable. Identical
        //    math on server and predicting client (determinism contract).
        const float h = TerrainHeightAt(pos[0], pos[2]);
        if (pos[1] < h)
        {
            pos[1] = h;
            if (vel[1] < 0.0f)
                vel[1] = 0.0f;
        }
    }

} // namespace Terrafront
