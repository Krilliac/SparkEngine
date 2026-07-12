/**
 * @file TFWorldSetup.cpp
 * @brief Scene/terrain load, WorldServer/AreaServer boot, origin-rebase driving.
 *
 * Terrain model: procedural heightfield — dune base + a canyon separating the
 * SW/SE quadrants + one flat plateau per region (from regions.json). Every
 * object in cindral_wastes.scene is authored at its region's plateau height,
 * so TerrainHeightAt() and the scene agree exactly at every build site.
 */
#include "World/TFWorldSetup.h"

#include "World/TFSanctuaryZone.h"
#include "World/TFWorldCollision.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponSystem.h"
#include "Net/TFServerSim.h"

#include "Spark/IEngineContext.h"
#include "SceneManager/SceneManager.h"
#include "Engine/ECS/Components.h"
#include "Engine/World/WorldOriginSystem.h"
#include "Camera/SparkEngineCamera.h"
#include "Game/GameObject.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Game/PlaceholderMesh.h"
#include "Game/TFComponents.h"
#include "Game/TFGroundFx.h"
#include "Game/TFBlobShadows.h" // W13 shadow-polish lane: ground-projected blob shadows
#include "Game/TFVehicleFx.h"   // W13 vehicle-damage-states lane: hull-hp smoke/spark/fire
#include "World/TFWeatherFx.h"    // W12 weather-visuals: storm cycle + client visuals
#include "World/TFRegionDecor.h"  // W12 decor-instancing: RenderInstanced opt-in
#include "World/TFRegionSystem.h" // W12 decor-instancing: Decor() accessor
#include "Game/TFImpactFx.h"      // impact-fx lane (W10)
#include "Game/TFGrenadeSystem.h" // grenades lane (W10): replicated grenade fx
#include "Game/TFPingSystem.h"    // ping-system lane (W11): world-space ping diamonds
#include "Game/TFOpticsSystem.h"  // W11 weapon-optics: scoped FOV zoom + hold-breath sway
#include "Game/TFViewModel.h"
#include "Game/TFTransparentPass.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/WorldServer.h"
#include "Engine/Networking/AreaServer.h"
#include "Engine/Networking/IAreaSimulation.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
#include <type_traits>

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

#ifdef ENABLE_NETWORKING
        /// Attach TFServerSim to the AreaServer tick if (and only if) it implements
        /// Spark::Net::IAreaSimulation. Template so this file compiles even while the
        /// TFServerSim agent has not landed the interface yet; at final build time
        /// the true branch is taken (frozen contract, DESIGN.md §2).
        template <typename TSim> void AttachSimulation(Spark::Net::AreaServer& area, TSim* sim)
        {
            if constexpr (std::is_base_of_v<Spark::Net::IAreaSimulation, TSim>)
            {
                area.SetSimulation(sim);
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFServerSim attached to AreaServer tick (%.0f Hz)",
                               kServerTickHz);
            }
            else
            {
                (void)area;
                (void)sim;
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] TFServerSim does not implement IAreaSimulation; area tick hook not attached");
            }
        }
#endif // ENABLE_NETWORKING

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

    // ---------------------------------------------------------------------------
    // Rendering — the module owns the frame (TerrafrontModule::OnRender)
    // ---------------------------------------------------------------------------

    void TFWorldSetup::CreateCamera()
    {
        GraphicsEngine* gfx = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetGraphics() : nullptr;
        if (!gfx)
            return; // headless / dedicated server: no camera, RenderWorld no-ops

        const float w = static_cast<float>(gfx->GetWindowWidth());
        const float h = static_cast<float>(gfx->GetWindowHeight());
        m_camera = std::make_unique<SparkEngineCamera>();
        m_camera->Initialize(h > 0.0f ? w / h : 16.0f / 9.0f);
        // 4 km continent: the 1000 m default far plane clips most of the map.
        m_camera->Console_SetClippingPlanes(0.3f, 6000.0f);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] module camera created (%ux%u)", gfx->GetWindowWidth(),
                       gfx->GetWindowHeight());
    }

    void TFWorldSetup::ComputeViewProj(DirectX::XMMATRIX& outView, DirectX::XMMATRIX& outProj) const
    {
        using namespace DirectX;

        // First person whenever the local pawn is alive: TFClientNet drives
        // m_camera's position (eye height) and mouse-look owns its rotation.
        // Matrices are built HERE from the camera pose so the module pins its
        // own FOV and clip planes (0.3–6000 m: the default camera far plane of
        // 1000 m clips most of the 4 km continent).
        if (m_camera && m_ctx && m_ctx->players && m_ctx->HasLocalPlayer())
        {
            PawnInfo pawn{};
            if (m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) && pawn.alive)
            {
                const XMFLOAT3 cp = m_camera->GetPosition();
                const XMFLOAT3 cf = m_camera->GetForward();
                const XMVECTOR eye = XMLoadFloat3(&cp);
                XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&cf));
                // W11 weapon-optics: scoped hold-breath sway — a sub-0.1-deg VISUAL-ONLY
                // rotation of the view basis. Fire rays are built from the unswayed camera
                // pose (TFWeaponSystem::BuildViewRay), so no aim effect is encoded here.
                float optSwayYaw = 0.0f, optSwayPitch = 0.0f;
                TFOpticsSystem::Get().CameraSwayRad(optSwayYaw, optSwayPitch);
                if (optSwayYaw != 0.0f || optSwayPitch != 0.0f)
                {
                    fwd = XMVector3Normalize(
                        XMVector3TransformNormal(fwd, XMMatrixRotationRollPitchYaw(optSwayPitch, optSwayYaw, 0.0f)));
                }
                outView = XMMatrixLookAtLH(eye, XMVectorAdd(eye, fwd), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

                float aspect = 16.0f / 9.0f;
                if (GraphicsEngine* gfx = m_ctx->engine ? m_ctx->engine->GetGraphics() : nullptr)
                {
                    const float h = static_cast<float>(gfx->GetWindowHeight());
                    if (h > 0.0f)
                        aspect = static_cast<float>(gfx->GetWindowWidth()) / h;
                }
                // W11 weapon-optics: ADS FOV zoom (1x hip -> 0.9x red dot / 0.25x 4x scope,
                // blended). TFNameplates mirrors this factor in its matching hardcoded
                // projection — keep them in sync.
                outProj = XMMatrixPerspectiveFovLH(XM_PIDIV4 * 1.6f * TFOpticsSystem::Get().CameraFovScale(), aspect,
                                                   0.3f, 6000.0f);
                return;
            }
        }

        // No local pawn: fixed overview across the dunes toward the MRA
        // skyanchor (region table when loaded; authored coordinates otherwise).
        float tx = 2048.0f, tz = 3600.0f;
        if (m_ctx && m_ctx->data && m_ctx->data->IsLoaded())
        {
            for (const RegionDef& r : m_ctx->data->GetContinent().regions)
            {
                if (r.tier == "skyanchor" && r.homeFaction == FactionId::MRA)
                {
                    tx = r.centerX;
                    tz = r.centerZ;
                    break;
                }
            }
        }
        const XMVECTOR eye = XMVectorSet(2048.0f, 80.0f, 1900.0f, 1.0f);
        const XMVECTOR at = XMVectorSet(tx, m_terrain.plateauSky + 8.0f, tz, 1.0f);
        outView = XMMatrixLookAtLH(eye, at, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

        float aspect = 16.0f / 9.0f;
        if (GraphicsEngine* gfx = m_ctx && m_ctx->engine ? m_ctx->engine->GetGraphics() : nullptr)
        {
            const float h = static_cast<float>(gfx->GetWindowHeight());
            if (h > 0.0f)
                aspect = static_cast<float>(gfx->GetWindowWidth()) / h;
        }
        outProj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.5f, 6000.0f);
    }

    Mesh* TFWorldSetup::GetOrLoadEcsMesh(const std::string& meshPath)
    {
        if (meshPath.empty() || !m_ctx || !m_ctx->engine)
            return nullptr;
        if (auto it = m_ecsMeshCache.find(meshPath); it != m_ecsMeshCache.end())
            return it->second.get();

        GraphicsEngine* gfx = m_ctx->engine->GetGraphics();
        if (!gfx || !gfx->GetDevice() || !gfx->GetContext())
            return nullptr;

        auto mesh = std::make_unique<Mesh>();
        // Same tinyobjloader path the scene geometry uses (device-direct); falls
        // back to a unit cube if the OBJ is missing/unparseable.
        LoadOrPlaceholderMesh(*mesh, gfx->GetDevice(), gfx->GetContext(),
                              std::wstring(meshPath.begin(), meshPath.end()));
        Mesh* raw = mesh.get();
        m_ecsMeshCache.emplace(meshPath, std::move(mesh));
        return raw;
    }

    void TFWorldSetup::DrawSkybox(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;
        if (!m_ctx || !m_ctx->engine || !m_camera)
            return;
        GraphicsEngine* gfx = m_ctx->engine->GetGraphics();
        if (!gfx || !gfx->GetDevice() || !gfx->GetContext())
            return;

        if (!m_skyMesh)
        {
            // Inward-facing unit cube: 6 faces, each 4 verts / 2 tris, wound so the
            // inside is front-facing. Face order matches the texture array below:
            // +X, -X, +Y, -Y, +Z, -Z.
            struct Face
            {
                XMFLOAT3 a, b, c, d;
                XMFLOAT3 n;
            };
            const Face faces[6] = {
                {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}, {-1, 0, 0}},    // +X
                {{-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}, {1, 0, 0}}, // -X
                {{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {0, -1, 0}},    // +Y (up)
                {{-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {0, 1, 0}}, // -Y (down)
                {{1, -1, 1}, {1, 1, 1}, {-1, 1, 1}, {-1, -1, 1}, {0, 0, -1}},    // +Z
                {{-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {1, -1, -1}, {0, 0, 1}}, // -Z
            };
            std::vector<Vertex> verts;
            std::vector<unsigned int> inds;
            verts.reserve(24);
            inds.reserve(36);
            for (const Face& f : faces)
            {
                const unsigned base = static_cast<unsigned>(verts.size());
                verts.push_back(Vertex(f.a, f.n, {0.0f, 1.0f}));
                verts.push_back(Vertex(f.b, f.n, {0.0f, 0.0f}));
                verts.push_back(Vertex(f.c, f.n, {1.0f, 0.0f}));
                verts.push_back(Vertex(f.d, f.n, {1.0f, 1.0f}));
                // Reverse winding so the INSIDE of the box is front-facing (the
                // camera sits at the center); otherwise back-face culling hides it.
                inds.push_back(base + 0);
                inds.push_back(base + 2);
                inds.push_back(base + 1);
                inds.push_back(base + 0);
                inds.push_back(base + 3);
                inds.push_back(base + 2);
            }
            m_skyMesh = std::make_unique<Mesh>();
            m_skyMesh->Initialize(gfx->GetDevice(), gfx->GetContext());
            m_skyMesh->CreateFromVertices(verts, inds);
        }
        if (!m_skyMesh || m_skyMesh->GetIndexCount() < 36)
            return;

        // Centered on the camera, huge but inside the far plane (corner ~5000 < 6000).
        const XMFLOAT3 cam = m_camera->GetPosition();

        // Per-zone sky: the sanctuary shows its own (orbital) sky; the continent
        // shows Veyra's. Selected by the camera's world position.
        const SkyboxDef& sky = TFTravel_IsInSanctuary(cam.x, cam.z) ? Pres().sanctuarySkybox : Pres().skybox;
        const XMMATRIX world =
            XMMatrixScaling(sky.scale, sky.scale, sky.scale) * XMMatrixTranslation(cam.x, cam.y, cam.z);

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        // Over-1.0 so the shader's ambient+N.L keeps the sky near full-bright.
        // W12 weather-visuals: storms dim the sky (basic path has no fog).
        const float skyDim = TFWeatherFx::Get().SkyboxDim();
        const XMFLOAT4 kSky{sky.tint[0] * skyDim, sky.tint[1] * skyDim, sky.tint[2] * skyDim, sky.tint[3]};
        for (int i = 0; i < 6; ++i)
        {
            gfx->UpdateBasicConstants(world, view, proj, kSky, XMFLOAT2(1, 1));
            gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV("Assets/" + sky.faceTex[i]));
            m_skyMesh->RenderRange(dc, static_cast<unsigned>(i * 6), 6);
        }
        gfx->SetBasicTexture(nullptr);
    }

    void TFWorldSetup::DrawTerrain(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;
        if (!m_ctx || !m_ctx->engine)
            return;
        GraphicsEngine* gfx = m_ctx->engine->GetGraphics();
        if (!gfx || !gfx->GetDevice() || !gfx->GetContext())
            return;

        if (!m_terrainMesh)
        {
            // Grid over the whole map; vertex Y sampled from TerrainHeightAt so the
            // relief matches the gameplay heightfield (dunes / canyon / plateaus).
            // Cells per side. Players are clamped to the ANALYTIC TerrainHeightAt,
            // so mesh interpolation error = visible feet/camera sinking. The worst
            // curvature is the plateau skirt (32 m smoothstep over 180 m): at
            // N=96 (42.7 m cells) the chord error is ~1.4 m; N=192 cuts it to
            // ~0.35 m for a one-time 37k-vert build. Keep >=192.
            constexpr int N = 192; // cells per side (193x193 verts)
            // Render-only sink: mesa-cube top faces are authored EXACTLY at the
            // plateau height, i.e. coplanar with this mesh — they z-fought and
            // "phased in and out" while walking. Drawing the terrain 4 cm low
            // separates the surfaces; gameplay height (TerrainHeightAt) untouched.
            constexpr float kVisualBiasM = 0.04f;
            const float size = m_ctx->data && m_ctx->data->IsLoaded() ? m_ctx->data->GetContinent().sizeM : 4096.0f;
            const float step = size / N;
            const float uvTiles = Pres().terrain.uvTiles; // texture repeats across the map
            const float e = step;                         // finite-difference epsilon for normals

            std::vector<Vertex> verts;
            std::vector<unsigned int> inds;
            verts.reserve((N + 1) * (N + 1));
            for (int j = 0; j <= N; ++j)
            {
                for (int i = 0; i <= N; ++i)
                {
                    const float x = i * step, z = j * step;
                    const float h = TerrainHeightAt(x, z) - kVisualBiasM;
                    const float hx0 = TerrainHeightAt(x - e, z), hx1 = TerrainHeightAt(x + e, z);
                    const float hz0 = TerrainHeightAt(x, z - e), hz1 = TerrainHeightAt(x, z + e);
                    XMVECTOR n = XMVector3Normalize(XMVectorSet(hx0 - hx1, 2.0f * e, hz0 - hz1, 0.0f));
                    XMFLOAT3 nf;
                    XMStoreFloat3(&nf, n);
                    verts.push_back(Vertex({x, h, z}, nf, {i / (float)N * uvTiles, j / (float)N * uvTiles}));
                }
            }
            const auto idx = [&](int i, int j) { return static_cast<unsigned>(j * (N + 1) + i); };
            inds.reserve(N * N * 6);
            for (int j = 0; j < N; ++j)
                for (int i = 0; i < N; ++i)
                {
                    inds.push_back(idx(i, j));
                    inds.push_back(idx(i, j + 1));
                    inds.push_back(idx(i + 1, j + 1));
                    inds.push_back(idx(i, j));
                    inds.push_back(idx(i + 1, j + 1));
                    inds.push_back(idx(i + 1, j));
                }
            m_terrainMesh = std::make_unique<Mesh>();
            m_terrainMesh->Initialize(gfx->GetDevice(), gfx->GetContext());
            m_terrainMesh->CreateFromVertices(verts, inds);
        }
        if (!m_terrainMesh || m_terrainMesh->GetIndexCount() < 6)
            return;

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV("Assets/" + Pres().terrain.texture));
        gfx->UpdateBasicConstants(XMMatrixIdentity(), view, proj, XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                                  XMFLOAT2(1.0f, 1.0f));
        m_terrainMesh->Render(dc);
        gfx->SetBasicTexture(nullptr);
    }

    void TFWorldSetup::DrawSceneObjects(SceneManager* sm, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
    {
        // Extracted from RenderWorld (continents lane) so the continent scene and
        // the additive sanctuary scene share ONE draw path — behavior for the
        // continent scene is byte-identical to the previous inline loop.
        if (!sm || !m_ctx || !m_ctx->engine)
            return;
        GraphicsEngine* gfx = m_ctx->engine->GetGraphics();
        if (!gfx || !gfx->GetDevice() || !gfx->GetContext())
            return;

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        const DirectX::XMFLOAT4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
        for (const auto& obj : sm->GetObjects())
        {
            if (!obj || !obj->IsActive() || !obj->IsVisible())
                continue;
            // Skip the flat ground plane (Terrain_Rock material) — the
            // procedural heightfield mesh replaces it.
            if (obj->GetMaterialPath().find("Terrain_Rock") != std::string::npos)
                continue;
            Mesh* mesh = obj->GetMesh();
            if (!mesh || mesh->GetVertexCount() == 0 || mesh->GetIndexCount() == 0)
                continue;

            // Scene material (material= key): albedo texture + UV tiling.
            const GraphicsEngine::BasicMaterial* sceneMat =
                obj->GetMaterialPath().empty() ? nullptr : gfx->GetOrLoadBasicMaterial(obj->GetMaterialPath());
            ID3D11ShaderResourceView* sceneSrv = sceneMat ? sceneMat->srv.Get() : nullptr;
            const DirectX::XMFLOAT2 sceneTiling = sceneMat ? sceneMat->tiling : DirectX::XMFLOAT2{1.0f, 1.0f};
            // W8 render-pbr-lite: bind the material's normal/roughness maps
            // (nullptr binds the flat identity defaults — unchanged look).
            gfx->SetBasicMaterialTextures(sceneMat ? sceneMat->normalSrv.Get() : nullptr,
                                          sceneMat ? sceneMat->roughnessSrv.Get() : nullptr);

            const DirectX::XMMATRIX world = obj->GetWorldMatrix();
            const auto& submeshes = mesh->GetSubmeshes();
            if (submeshes.empty())
            {
                // Primitive (terrain plane, mesa cubes): scene material over whole mesh
                gfx->UpdateBasicConstants(world, view, proj, kWhite, sceneTiling);
                gfx->SetBasicTexture(sceneSrv);
                mesh->Render(dc);
            }
            else
            {
                // OBJ model: draw each MTL material range. Ranges with
                // their own map_Kd (e.g. Kenney colormap, Quaternius trim
                // sheets) use it at 1:1 UVs; untextured ranges fall back
                // to the scene material texture tinted by the MTL Kd.
                for (const MeshSubmesh& smesh : submeshes)
                {
                    ID3D11ShaderResourceView* srv = nullptr;
                    DirectX::XMFLOAT2 tiling{1.0f, 1.0f};
                    if (!smesh.diffuseTexture.empty())
                    {
                        srv = gfx->GetOrLoadTextureSRV(smesh.diffuseTexture);
                    }
                    if (!srv)
                    {
                        srv = sceneSrv;
                        tiling = sceneTiling;
                    }
                    gfx->UpdateBasicConstants(world, view, proj, smesh.diffuseColor, tiling);
                    gfx->SetBasicTexture(srv);
                    mesh->RenderRange(dc, smesh.indexStart, smesh.indexCount);
                }
            }
        }
        // Restore default texture binding for subsequent draw paths
        gfx->SetBasicTexture(nullptr);
        gfx->SetBasicMaterialTextures(nullptr, nullptr); // W8 pbr-lite: flat defaults
    }

    void TFWorldSetup::RenderWorld()
    {
        if (!m_initialized || !m_ctx || !m_ctx->engine)
            return;
        GraphicsEngine* gfx = m_ctx->engine->GetGraphics();
        if (!gfx)
            return;

        // Mirrors GameModules/SparkGameFPS Game::Render: the ONLY
        // BeginFrame/EndFrame pair per frame while this module is loaded.
        gfx->BeginFrame();
        try
        {
            DirectX::XMMATRIX view, proj;
            ComputeViewProj(view, proj);

            // Per-frame constants (b1): the basic pixel shader reads the
            // directional/ambient light from this buffer — without this call it
            // stays unwritten and everything shades to black.
            {
                const DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
                DirectX::XMFLOAT3 camPos;
                DirectX::XMStoreFloat3(&camPos, invView.r[3]);
                gfx->UpdateFrameConstants(view, proj, camPos);
            }

            // 0) Skybox first, so the world's opaque geometry overdraws it.
            DrawSkybox(view, proj);

            // 0b) Procedural terrain relief (replaces the flat scene ground plane).
            DrawTerrain(view, proj);

            // 1) Scene geometry (terrain plane, mesas, buildings) — the continent
            //    scene plus the additive Sanctuary Haven zone (continents lane).
            //    Draws are issued through GraphicsEngine/Mesh MEMBER functions:
            //    GameObject::Render() reads EngineContext::Get(), which is a
            //    per-image global and unset inside this statically-linked DLL.
            DrawSceneObjects(m_scene, view, proj);
            DrawSceneObjects(m_sanctuaryScene.get(), view, proj);

            // 2) ECS visuals — pawns, vehicles and deployables all attach a
            //    MeshRenderer (TFPlayerSystem::AttachPawnVisual & friends). The
            //    engine's SubmitMeshForRendering/ProcessDrawList path loads meshes
            //    through the AssetPipeline (OBJ loader unreliable on Windows), so
            //    pawns never drew. Draw them through the SAME device-direct mesh +
            //    basic-shader path the scene geometry above uses.
            if (World* world = m_ctx->engine->GetWorld())
            {
                ID3D11DeviceContext* dc = gfx->GetContext();
                gfx->SetBasicShaders();
                const auto& registry = world->GetRegistry();
                auto ecsView = world->GetEntitiesWith<Transform, MeshRenderer>();
                for (auto entity : ecsView)
                {
                    const MeshRenderer& mr = ecsView.get<MeshRenderer>(entity);
                    if (!mr.visible)
                        continue;
                    if (const auto* active = registry.try_get<ActiveComponent>(entity); active && !active->active)
                        continue;

                    Mesh* mesh = GetOrLoadEcsMesh(mr.meshPath);
                    if (!mesh || mesh->GetVertexCount() == 0 || mesh->GetIndexCount() == 0)
                        continue;

                    const GraphicsEngine::BasicMaterial* mat =
                        mr.materialPath.empty() ? nullptr : gfx->GetOrLoadBasicMaterial(mr.materialPath);
                    ID3D11ShaderResourceView* matSrv = mat ? mat->srv.Get() : nullptr;
                    const DirectX::XMFLOAT2 matTiling = mat ? mat->tiling : DirectX::XMFLOAT2{1.0f, 1.0f};
                    // W8 render-pbr-lite: per-material normal/roughness (defaults on nullptr).
                    gfx->SetBasicMaterialTextures(mat ? mat->normalSrv.Get() : nullptr,
                                                  mat ? mat->roughnessSrv.Get() : nullptr);

                    const DirectX::XMMATRIX worldM = ecsView.get<Transform>(entity).GetWorldMatrix(registry);
                    const auto& submeshes = mesh->GetSubmeshes();

                    // Faction tint: multiply the model's own texture/color by a
                    // moderated faction hue so friend/foe read at a glance across
                    // the battlefield. lerp(white, factionColor, 0.55) keeps the
                    // colormap legible while clearly coloring the body.
                    DirectX::XMFLOAT4 tint{1.0f, 1.0f, 1.0f, 1.0f};
                    if (const auto* fc = registry.try_get<TFFactionComp>(entity); fc && fc->faction != FactionId::None)
                    {
                        float fcol[4];
                        FactionColor(fc->faction, fcol);
                        constexpr float k = 0.40f; // moderate tint; keep the body bright/legible
                        tint.x = 1.0f + k * (fcol[0] - 1.0f);
                        tint.y = 1.0f + k * (fcol[1] - 1.0f);
                        tint.z = 1.0f + k * (fcol[2] - 1.0f);
                    }

                    const float emissive = mr.emissive; // 0 == unchanged (plain path)
                    if (submeshes.empty())
                    {
                        gfx->UpdateBasicConstants(worldM, view, proj, tint, matTiling, emissive);
                        gfx->SetBasicTexture(matSrv);
                        mesh->Render(dc);
                    }
                    else
                    {
                        for (const MeshSubmesh& smesh : submeshes)
                        {
                            ID3D11ShaderResourceView* srv =
                                smesh.diffuseTexture.empty() ? nullptr : gfx->GetOrLoadTextureSRV(smesh.diffuseTexture);
                            DirectX::XMFLOAT2 tiling{1.0f, 1.0f};
                            if (!srv)
                            {
                                srv = matSrv;
                                tiling = matTiling;
                            }
                            const DirectX::XMFLOAT4 c{smesh.diffuseColor.x * tint.x, smesh.diffuseColor.y * tint.y,
                                                      smesh.diffuseColor.z * tint.z, smesh.diffuseColor.w};
                            gfx->UpdateBasicConstants(worldM, view, proj, c, tiling, emissive);
                            gfx->SetBasicTexture(srv);
                            mesh->RenderRange(dc, smesh.indexStart, smesh.indexCount);
                        }
                    }
                }
                gfx->SetBasicTexture(nullptr);
                gfx->SetBasicMaterialTextures(nullptr, nullptr); // W8 pbr-lite: flat defaults
            }

            // 2b) W12 decor-instancing: grouped region decor — one
            //     DrawMeshInstanced per (mesh, material, emissive) group.
            //     Small groups (< 4) and everything else stayed in the
            //     per-entity loop above; falls back automatically when the
            //     engine lacks the instanced pipeline. Uses this frame's
            //     view/proj and the b1 constants set at the top of RenderWorld.
            if (m_ctx->regions)
            {
                if (TFRegionDecor* decor = m_ctx->regions->Decor())
                    decor->RenderInstanced(gfx, view, proj);
            }

            // 3) First-person viewmodel — arms + equipped weapon with sway/walk
            //    bob/recoil (Game/TFViewModel.h; supersedes the old static draw —
            //    keeping both double-draws the gun).
            if (m_ctx->HasLocalPlayer() && m_ctx->weapons)
                TFViewModel::Get().Render(*m_ctx, gfx, view, proj);

            // 4) Shot effects: bright unlit muzzle flash + tracer per recent shot.
            //    Drawn as stretched unit cubes; the over-1.0 color clamps bright
            //    even after the basic shader's lighting multiply.
            if (!m_shotFx.empty())
            {
                if (!m_fxCube)
                {
                    m_fxCube = std::make_unique<Mesh>();
                    m_fxCube->Initialize(gfx->GetDevice(), gfx->GetContext());
                    m_fxCube->CreateCube(1.0f);
                }
                if (m_fxCube && m_fxCube->GetIndexCount() > 0)
                {
                    ID3D11DeviceContext* dc = gfx->GetContext();
                    gfx->SetBasicShaders();
                    gfx->SetBasicTexture(nullptr);
                    using namespace DirectX;
                    const MuzzleFxDef& fxDef = Pres().muzzleFx;
                    for (const ShotFx& fx : m_shotFx)
                    {
                        const double age = m_fxClock - fx.t0;
                        XMVECTOR o = XMLoadFloat3(&fx.origin);
                        XMVECTOR f = XMVector3Normalize(XMLoadFloat3(&fx.dir));
                        XMVECTOR upRef =
                            (std::fabs(XMVectorGetY(f)) > 0.99f) ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
                        XMVECTOR r = XMVector3Normalize(XMVector3Cross(upRef, f));
                        XMVECTOR u = XMVector3Cross(f, r);
                        XMMATRIX orient = XMMatrixIdentity();
                        orient.r[0] = r;
                        orient.r[1] = u;
                        orient.r[2] = f;

                        // Muzzle is at the gun tip (lower-right of view, ~1.2 m
                        // forward) so effects clear the 0.5 m near plane and line up
                        // with the first-person weapon instead of the eye.
                        XMVECTOR muzzle = o + f * fxDef.muzzleForwardM + r * fxDef.muzzleRightM + u * fxDef.muzzleUpM;

                        // Tracer: thin bright streak from the muzzle forward. Edge-on
                        // from the shooter, but a clear streak from any side angle.
                        if (age < fxDef.tracerLifeSec)
                        {
                            const float kLen = fxDef.tracerLenM, kThick = fxDef.tracerThickM;
                            XMVECTOR mid = XMVectorAdd(muzzle, XMVectorScale(f, kLen * 0.5f));
                            XMMATRIX w =
                                XMMatrixScaling(kThick, kThick, kLen) * orient * XMMatrixTranslationFromVector(mid);
                            gfx->UpdateBasicConstants(w, view, proj,
                                                      XMFLOAT4(fxDef.tracerColor[0], fxDef.tracerColor[1],
                                                               fxDef.tracerColor[2], fxDef.tracerColor[3]),
                                                      XMFLOAT2(1, 1));
                            m_fxCube->Render(dc);
                        }
                        // Muzzle flash: bright puff at the gun tip (first ~50 ms).
                        if (age < fxDef.flashLifeSec)
                        {
                            XMMATRIX w =
                                XMMatrixScaling(fxDef.flashScale[0], fxDef.flashScale[1], fxDef.flashScale[2]) *
                                orient * XMMatrixTranslationFromVector(muzzle);
                            gfx->UpdateBasicConstants(w, view, proj,
                                                      XMFLOAT4(fxDef.flashColor[0], fxDef.flashColor[1],
                                                               fxDef.flashColor[2], fxDef.flashColor[3]),
                                                      XMFLOAT2(1, 1));
                            m_fxCube->Render(dc);
                        }
                    }
                    gfx->SetBasicTexture(nullptr);
                }
            }

            // 4b) Blob shadows (W13 shadow-polish lane): soft dark ground discs
            //     under live pawns/vehicles/deployables — the basic path's scoped
            //     stand-in for a real shadow map (Game/TFBlobShadows.h; cap 128,
            //     camera-distance culled, height-above-ground faded). Drawn before
            //     the additive ground/impact puffs so the shadow reads under them.
            if (m_ctx->HasLocalPlayer())
                TFBlobShadows::Get().UpdateAndRender(*m_ctx, gfx, view, proj);

            // 5) Hover dust: client-side flipbook ground puffs under fast-moving
            //    vehicles (Game/TFGroundFx.h; camera-facing additive quads, cap 64).
            if (m_ctx->HasLocalPlayer())
                TFGroundFx::Get().UpdateAndRender(*m_ctx, gfx, view, proj);

            // 5b) Bullet-impact bursts: pooled flipbook quads at local/remote
            //     shot impact points (Game/TFImpactFx.h; cap 48, ~0.15 s).
            if (m_ctx->HasLocalPlayer())
                TFImpactFx::Get().UpdateAndRender(*m_ctx, gfx, view, proj);

            // 5b2) Vehicle damage FX (W13 vehicle-damage-states lane): hull-hp
            //      smoke/spark/fire billboards + a critical-tier pitched-down
            //      engine cue on damaged hulls (Game/TFVehicleFx.h; reads
            //      TFVehicleInfo.hp via the existing ForEachVehicle enumerator).
            if (m_ctx->HasLocalPlayer())
                TFVehicleFx::Get().UpdateAndRender(*m_ctx, gfx, view, proj);

            // 5c) Grenades (W10): replicated grenade bodies + boom flipbook
            //     (Game/TFGrenadeSystem.h; restores blend/depth/texture state).
            if (m_ctx->HasLocalPlayer() && m_ctx->grenades)
                m_ctx->grenades->RenderClientFx(gfx, view, proj);

            // 5d) Pings (W11): squad-scoped tactical ping diamonds
            //     (Game/TFPingSystem.h; restores blend/depth/texture state).
            if (m_ctx->HasLocalPlayer() && m_ctx->pings)
                m_ctx->pings->RenderClientFx(gfx, view, proj);

            // 6) Transparent surfaces (shield-wall energy planes + any queued
            //    alpha FX) — sorted back-to-front and drawn AFTER all opaque
            //    passes (W8 render-transparency lane; producers queue during
            //    their Update, e.g. TFDeployableSystem's shield walls).
            TFTransparentPass::Get().Flush(gfx, view, proj);

            // 7) Dust storm (W12): wind-blown dust billboards + fullscreen sandy
            //    tint, drawn last so the wash covers transparents too
            //    (World/TFWeatherFx.h; cap 120, restores blend/depth/texture).
            if (m_ctx->HasLocalPlayer())
                TFWeatherFx::Get().Render(*m_ctx, gfx, view, proj);
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "TFRender", 5, "[TF] RenderWorld exception: %s", e.what());
        }
        catch (...)
        {
            SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "TFRender", 5, "[TF] RenderWorld: unknown exception");
        }
        gfx->EndFrame();
    }

    // ---------------------------------------------------------------------------
    // Networking boot (frozen API)
    // ---------------------------------------------------------------------------

    bool TFWorldSetup::StartHost(uint16_t port)
    {
#ifdef ENABLE_NETWORKING
        return BootServer(port, NetRole::ListenHost);
#else
        (void)port;
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] StartHost: built without ENABLE_NETWORKING");
        return false;
#endif
    }

    bool TFWorldSetup::StartDedicated(uint16_t port)
    {
#ifdef ENABLE_NETWORKING
        return BootServer(port, NetRole::DedicatedServer);
#else
        (void)port;
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] StartDedicated: built without ENABLE_NETWORKING");
        return false;
#endif
    }

    bool TFWorldSetup::Connect(const std::string& ip, uint16_t port)
    {
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() && !nm.Initialize())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Connect: NetworkManager init failed");
            return false;
        }
        if (!nm.Connect(ip, port, "TerrafrontPlayer"))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Connect to %s:%u failed", ip.c_str(), port);
            return false;
        }
        m_netBooted = true;
        m_ctx->role = NetRole::Client;
        // TFClientNet observes the NetworkManager connection in its Update and
        // runs the TF handshake (WorldWelcome / FactionSelect / SpawnRequest).
        Spark::SimpleConsole::GetInstance().LogInfo("[TF] Connecting to " + ip + ":" + std::to_string(port));
        return true;
#else
        (void)ip;
        (void)port;
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Connect: built without ENABLE_NETWORKING");
        return false;
#endif
    }

#ifdef ENABLE_NETWORKING

    bool TFWorldSetup::BootServer(uint16_t port, NetRole role)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() && !nm.Initialize())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] BootServer: NetworkManager init failed");
            return false;
        }
        if (!nm.StartServer(port, static_cast<int>(kMaxPlayers)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] BootServer: StartServer on port %u failed", port);
            return false;
        }

        // WorldServer + ONE AreaServer covering the whole 4096m continent
        // (DESIGN.md §2 topology; region hexes are game logic, not areas).
        m_worldServer = std::make_unique<Spark::Net::WorldServer>();
        Spark::Net::WorldServerConfig wc{};
        wc.worldName = "TERRAFRONT " + std::string("Cindral Wastes");
        wc.port = static_cast<uint16_t>(port + 1);
        wc.interServerPort = static_cast<uint16_t>(port + 2);
        wc.maxTotalClients = static_cast<int>(kMaxPlayers);
        wc.tickRate = 10.0f;
        wc.enableLoadBalancing = false; // single area, nothing to balance
        if (!m_worldServer->Start(wc))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] WorldServer failed to start (continuing: NetworkManager carries gameplay)");
            m_worldServer.reset();
        }

        Spark::Net::AreaServerConfig ac{};
        ac.areaId = 1;
        ac.areaName = "CindralWastes";
        ac.scenePath = m_scenePath;
        ac.port = static_cast<uint16_t>(port + 3);
        ac.interServerPort = static_cast<uint16_t>(port + 4);
        ac.tickRate = kServerTickHz;
        ac.maxClients = static_cast<int>(kMaxPlayers);
        ac.enableAI = false;
        ac.enablePhysics = true;
        ac.enableScripting = false;
        if (m_worldServer)
            m_worldServer->RegisterAreaServer(ac);

        m_areaServer = std::make_unique<Spark::Net::AreaServer>();
        if (m_areaServer->Start(ac))
        {
            if (m_ctx->serverSim)
                AttachSimulation(*m_areaServer, m_ctx->serverSim);
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] AreaServer failed to start");
            m_areaServer.reset();
        }

        m_knownClients.clear();
        m_netBooted = true;
        m_ctx->role = role;

        const char* roleName = (role == NetRole::DedicatedServer) ? "dedicated server" : "listen host";
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Serving Cindral Wastes as %s on port %u", roleName, port);
        Spark::SimpleConsole::GetInstance().LogInfo("[TF] " + std::string(roleName) + " up on port " +
                                                    std::to_string(port));
        return true;
    }

    void TFWorldSetup::BridgeWorldServerSessions()
    {
        if (!m_worldServer || !m_worldServer->IsRunning())
            return;

        auto& nm = Spark::Net::NetworkManager::GetInstance();
        const auto& clients = nm.GetClients();

        for (const auto& [clientId, info] : clients)
        {
            if (m_knownClients.insert(clientId).second)
                m_worldServer->HandlePlayerConnect(clientId, info.name, {0.0f, 0.0f, 0.0f});
        }
        for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
        {
            if (clients.find(*it) == clients.end())
            {
                m_worldServer->HandlePlayerDisconnect(*it);
                it = m_knownClients.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void TFWorldSetup::StopNetworking()
    {
        if (m_areaServer)
        {
            m_areaServer->Stop();
            m_areaServer.reset();
        }
        if (m_worldServer)
        {
            m_worldServer->Stop();
            m_worldServer.reset();
        }
        if (m_netBooted)
        {
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (nm.GetRole() == Spark::Net::NetworkRole::Server)
                nm.StopServer();
            else
                nm.Disconnect();
            nm.Shutdown();
            m_netBooted = false;
        }
        m_knownClients.clear();
        if (m_ctx)
            m_ctx->role = NetRole::Standalone;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Frame driving
    // ---------------------------------------------------------------------------

    void TFWorldSetup::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

#ifdef ENABLE_NETWORKING
        if (m_netBooted)
        {
            // Single message pump for the module: TFWorldSetup booted the
            // NetworkManager, so it owns the Update (mirrors MMOWorldSetup).
            Spark::Net::NetworkManager::GetInstance().Update(deltaTime);
            if (m_ctx->IsAuthority())
                BridgeWorldServerSessions();
        }
#else
        (void)deltaTime;
#endif

        if (m_ctx->role == NetRole::Client)
            DriveOriginRebase();

        // W12 weather-visuals: server-owned dust-storm cycle + 0x547C sync +
        // pure-client handler poll (all roles; presentation reads the singleton).
        TFWeatherFx::Get().Update(*m_ctx, deltaTime);

        // audio-polish lane (W8): TFAudioAmbience owns the ambient beds
        // (sanctuary hum <-> wind crossfade); the old single-wind-loop starter
        // (MaybeStartAmbientAudio) was removed as dead code in W9.

        m_fxClock += deltaTime;
        // Reap expired shot effects (longest-lived component is the tracer).
        constexpr double kFxMaxLife = 0.10;
        m_shotFx.erase(std::remove_if(m_shotFx.begin(), m_shotFx.end(),
                                      [&](const ShotFx& fx) { return m_fxClock - fx.t0 > kFxMaxLife; }),
                       m_shotFx.end());
    }

    void TFWorldSetup::SpawnMuzzleFx(const float origin[3], const float dir[3])
    {
        if (!m_ctx->HasLocalPlayer())
            return;
        if (m_shotFx.size() > 64) // safety cap
            return;
        ShotFx fx;
        fx.t0 = m_fxClock;
        fx.origin = {origin[0], origin[1], origin[2]};
        fx.dir = {dir[0], dir[1], dir[2]};
        m_shotFx.push_back(fx);
    }

    void TFWorldSetup::DriveOriginRebase()
    {
        if (!m_origin || !m_ctx->engine || !m_ctx->players)
            return;
        if (m_ctx->localPlayer == kInvalidPlayer)
            return;

        World* world = m_ctx->engine->GetWorld();
        if (!world)
            return;

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
            return;

        m_origin->Update(world->GetRegistry(), DirectX::XMFLOAT3{pawn.pos[0], pawn.pos[1], pawn.pos[2]});
    }

    void TFWorldSetup::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // authoritative sim runs in TFServerSim (AreaServer tick)
    }

    void TFWorldSetup::Shutdown()
    {
        if (!m_initialized)
            return;
#ifdef ENABLE_NETWORKING
        StopNetworking();
#endif
        m_sanctuaryCollision.reset(); // removes its static bodies from the engine PhysicsSystem
        m_collision.reset();          // removes its static bodies from the engine PhysicsSystem
        m_camera.reset();
        m_scene = nullptr;
        m_ownScene.reset();
        m_sanctuaryScene.reset();
        m_sanctuaryLoaded = false;
        m_origin.reset();
        m_sceneLoaded = false;
        m_initialized = false;
    }

    void TFWorldSetup::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF World"))
            return;

        static const char* roleNames[] = {"Standalone", "ListenHost", "DedicatedServer", "Client"};
        ImGui::Text("Role: %s", roleNames[static_cast<int>(m_ctx->role)]);
        ImGui::Text("Scene: %s (%s)", m_scenePath.c_str(), m_sceneLoaded ? "loaded" : "not loaded");
        ImGui::Text("Height @ center (2048,2048): %.1f m", TerrainHeightAt(2048.0f, 2048.0f));
        ImGui::Text("World collision: %s (%zu static bodies)",
                    (m_collision && m_collision->IsActive()) ? "Jolt live" : "inactive",
                    m_collision ? m_collision->BodyCount() : size_t{0});
        ImGui::Text("Sanctuary: scene %s | collision %s (%zu bodies) | pad height %.1f m",
                    m_sanctuaryLoaded ? "loaded" : "not loaded",
                    (m_sanctuaryCollision && m_sanctuaryCollision->IsActive()) ? "Jolt live" : "inactive",
                    m_sanctuaryCollision ? m_sanctuaryCollision->BodyCount() : size_t{0},
                    TerrainHeightAt(kTFSanctuaryCenterX, kTFSanctuaryCenterZ));

#ifdef ENABLE_NETWORKING
        ImGui::Text("Net booted: %s | WorldServer: %s | AreaServer: %s", m_netBooted ? "yes" : "no",
                    (m_worldServer && m_worldServer->IsRunning()) ? "up" : "down",
                    (m_areaServer && m_areaServer->IsRunning()) ? "up" : "down");
        ImGui::Text("Sessions bridged: %zu", m_knownClients.size());
#endif

        if (m_origin)
        {
            const auto& stats = m_origin->GetStats();
            ImGui::Text("Origin rebases: %u (max dist %.0f m)", stats.totalRebases, stats.maxDistanceFromOrigin);
        }
#endif
    }

} // namespace Terrafront
