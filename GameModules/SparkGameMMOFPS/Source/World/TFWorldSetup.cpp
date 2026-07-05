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

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponSystem.h"
#include "Net/TFServerSim.h"

#include "Spark/IEngineContext.h"
#include "Audio/AudioEngine.h"
#include "SceneManager/SceneManager.h"
#include "Engine/ECS/Components.h"
#include "Engine/World/WorldOriginSystem.h"
#include "Camera/SparkEngineCamera.h"
#include "Game/GameObject.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Game/PlaceholderMesh.h"
#include "Game/TFComponents.h"
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

namespace Terrafront {

namespace {

constexpr const char* kFallbackScenePath = "Assets/Scenes/MMOFPS/cindral_wastes.scene";
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
template <typename TSim>
void AttachSimulation(Spark::Net::AreaServer& area, TSim* sim)
{
    if constexpr (std::is_base_of_v<Spark::Net::IAreaSimulation, TSim>) {
        area.SetSimulation(sim);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFServerSim attached to AreaServer tick (%.0f Hz)",
                       kServerTickHz);
    } else {
        (void)area;
        (void)sim;
        SPARK_LOG_WARN(Spark::LogCategory::Game,
                       "[TF] TFServerSim does not implement IAreaSimulation; area tick hook not attached");
    }
}
#endif // ENABLE_NETWORKING

} // namespace

TFWorldSetup::TFWorldSetup() = default;
TFWorldSetup::~TFWorldSetup() { if (m_initialized) Shutdown(); }

bool TFWorldSetup::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;

    m_origin = std::make_unique<Spark::World::WorldOriginSystem>();
    m_origin->SetRebasingThreshold(kOriginRebaseThreshold);
    m_origin->SetEnabled(true);

    LoadSceneAndTerrain();
    CreateCamera();

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
    if (!sm && m_ctx->engine && m_ctx->engine->GetGraphics()) {
        m_ownScene = std::make_unique<SceneManager>(m_ctx->engine->GetGraphics(),
                                                    m_ctx->engine->GetInput());
        sm = m_ownScene.get();
    }
    m_scene = sm;
    if (sm) {
        std::wstring wpath(m_scenePath.begin(), m_scenePath.end());
        m_sceneLoaded = sm->LoadScene(wpath);
        Spark::SimpleConsole::GetInstance().LogInfo(
            std::string("[TF] scene ") + m_scenePath + (m_sceneLoaded ? " loaded (" : " FAILED (") +
            std::to_string(sm->GetObjects().size()) + " objects)");
        if (!m_sceneLoaded)
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] Scene load failed: %s (headless or missing file?)",
                           m_scenePath.c_str());
    } else {
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] No SceneManager (headless server) - skipping visual scene load");
    }
    // TF-W2: feed the heightfield into ClipmapTerrain/TerrainRenderer so the
    // visual terrain matches TerrainHeightAt instead of the flat ground plane.
}

void TFWorldSetup::ParseTerrainParams(const std::string& scenePath)
{
    std::ifstream f(scenePath);
    if (!f.is_open()) {
        SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] Cannot read %s for terrain params; using defaults",
                       scenePath.c_str());
        return;
    }

    std::string line;
    bool inTerrain = false;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
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

        if      (key == "tfBaseHeight")       m_terrain.baseHeight = val;
        else if (key == "tfDuneAmp")          m_terrain.duneAmp = val;
        else if (key == "tfDunePeriodX")      m_terrain.dunePeriodX = val;
        else if (key == "tfDunePeriodZ")      m_terrain.dunePeriodZ = val;
        else if (key == "tfRidgeAmp")         m_terrain.ridgeAmp = val;
        else if (key == "tfCanyonX")          m_terrain.canyonX = val;
        else if (key == "tfCanyonHalfWidth")  m_terrain.canyonHalfW = val;
        else if (key == "tfCanyonZ0")         m_terrain.canyonZ0 = val;
        else if (key == "tfCanyonZ1")         m_terrain.canyonZ1 = val;
        else if (key == "tfCanyonDepth")      m_terrain.canyonDepth = val;
        else if (key == "tfPlateauRadius")    m_terrain.plateauRadius = val;
        else if (key == "tfPlateauSkirt")     m_terrain.plateauSkirt = val;
        else if (key == "tfPlateauSkyanchor") m_terrain.plateauSky = val;
        else if (key == "tfPlateauFort")      m_terrain.plateauFort = val;
        else if (key == "tfPlateauFacility")  m_terrain.plateauFacility = val;
        else if (key == "tfPlateauOutpost")   m_terrain.plateauOutpost = val;
    }
}

float TFWorldSetup::PlateauHeight(const std::string& tier) const
{
    if (tier == "skyanchor") return m_terrain.plateauSky;
    if (tier == "fort")      return m_terrain.plateauFort;
    if (tier == "facility")  return m_terrain.plateauFacility;
    return m_terrain.plateauOutpost;
}

float TFWorldSetup::TerrainHeightAt(float x, float z) const
{
    const TFTerrainParams& p = m_terrain;

    // Dune base
    float h = p.baseHeight
            + p.duneAmp * std::sin(x * p.dunePeriodX) * std::cos(z * p.dunePeriodZ)
            + p.ridgeAmp * std::sin(x * 0.013f + z * 0.011f);

    // Canyon between SW (AUC) and SE (HLX) territory
    const float nx = (x - p.canyonX) / p.canyonHalfW;
    if (std::fabs(nx) < 1.0f) {
        const float across = 1.0f - nx * nx;
        const float along = SmoothStep(p.canyonZ0 - 300.0f, p.canyonZ0, z)
                          * (1.0f - SmoothStep(p.canyonZ1, p.canyonZ1 + 300.0f, z));
        h -= p.canyonDepth * across * along;
    }

    // Flat mesa plateau around each region center (scene objects sit at
    // exactly these heights). Regions are far apart relative to the skirt,
    // so sequential blending is order-independent in practice.
    if (m_ctx && m_ctx->data && m_ctx->data->IsLoaded()) {
        for (const RegionDef& r : m_ctx->data->GetContinent().regions) {
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
    return h;
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
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] module camera created (%ux%u)",
                   gfx->GetWindowWidth(), gfx->GetWindowHeight());
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
            const XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&cf));
            outView = XMMatrixLookAtLH(eye, XMVectorAdd(eye, fwd),
                                       XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

            float aspect = 16.0f / 9.0f;
            if (GraphicsEngine* gfx = m_ctx->engine ? m_ctx->engine->GetGraphics() : nullptr)
            {
                const float h = static_cast<float>(gfx->GetWindowHeight());
                if (h > 0.0f)
                    aspect = static_cast<float>(gfx->GetWindowWidth()) / h;
            }
            outProj = XMMatrixPerspectiveFovLH(XM_PIDIV4 * 1.6f, aspect, 0.3f, 6000.0f);
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
    const XMVECTOR at  = XMVectorSet(tx, m_terrain.plateauSky + 8.0f, tz, 1.0f);
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

        // 1) Scene geometry (terrain plane, mesas, buildings). Draws are
        //    issued here through GraphicsEngine/Mesh MEMBER functions:
        //    GameObject::Render() reads EngineContext::Get(), which is a
        //    per-image global and unset inside this statically-linked DLL.
        if (SceneManager* sm = m_scene)
        {
            ID3D11DeviceContext* dc = gfx->GetContext();
            gfx->SetBasicShaders();
            const DirectX::XMFLOAT4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
            for (const auto& obj : sm->GetObjects())
            {
                if (!obj || !obj->IsActive() || !obj->IsVisible())
                    continue;
                Mesh* mesh = obj->GetMesh();
                if (!mesh || mesh->GetVertexCount() == 0 || mesh->GetIndexCount() == 0)
                    continue;

                // Scene material (material= key): albedo texture + UV tiling.
                const GraphicsEngine::BasicMaterial* sceneMat =
                    obj->GetMaterialPath().empty() ? nullptr : gfx->GetOrLoadBasicMaterial(obj->GetMaterialPath());
                ID3D11ShaderResourceView* sceneSrv = sceneMat ? sceneMat->srv.Get() : nullptr;
                const DirectX::XMFLOAT2 sceneTiling = sceneMat ? sceneMat->tiling : DirectX::XMFLOAT2{1.0f, 1.0f};

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
        }

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
                if (const auto* active = registry.try_get<ActiveComponent>(entity);
                    active && !active->active)
                    continue;

                Mesh* mesh = GetOrLoadEcsMesh(mr.meshPath);
                if (!mesh || mesh->GetVertexCount() == 0 || mesh->GetIndexCount() == 0)
                    continue;

                const GraphicsEngine::BasicMaterial* mat =
                    mr.materialPath.empty() ? nullptr : gfx->GetOrLoadBasicMaterial(mr.materialPath);
                ID3D11ShaderResourceView* matSrv = mat ? mat->srv.Get() : nullptr;
                const DirectX::XMFLOAT2 matTiling = mat ? mat->tiling : DirectX::XMFLOAT2{1.0f, 1.0f};

                const DirectX::XMMATRIX worldM = ecsView.get<Transform>(entity).GetWorldMatrix(registry);
                const auto& submeshes = mesh->GetSubmeshes();

                // Faction tint: multiply the model's own texture/color by a
                // moderated faction hue so friend/foe read at a glance across
                // the battlefield. lerp(white, factionColor, 0.55) keeps the
                // colormap legible while clearly coloring the body.
                DirectX::XMFLOAT4 tint{1.0f, 1.0f, 1.0f, 1.0f};
                if (const auto* fc = registry.try_get<TFFactionComp>(entity);
                    fc && fc->faction != FactionId::None)
                {
                    float fcol[4];
                    FactionColor(fc->faction, fcol);
                    constexpr float k = 0.55f;
                    tint.x = 1.0f + k * (fcol[0] - 1.0f);
                    tint.y = 1.0f + k * (fcol[1] - 1.0f);
                    tint.z = 1.0f + k * (fcol[2] - 1.0f);
                }

                if (submeshes.empty())
                {
                    gfx->UpdateBasicConstants(worldM, view, proj, tint, matTiling);
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
                        gfx->UpdateBasicConstants(worldM, view, proj, c, tiling);
                        gfx->SetBasicTexture(srv);
                        mesh->RenderRange(dc, smesh.indexStart, smesh.indexCount);
                    }
                }
            }
            gfx->SetBasicTexture(nullptr);
        }

        // 3) First-person weapon viewmodel — the equipped weapon drawn at a
        //    fixed offset from the camera so the player sees their gun. Placed
        //    in view space (right/down/forward) then transformed to world via
        //    inverse(view); empty model (dead / no pawn) draws nothing.
        if (m_ctx->HasLocalPlayer() && m_ctx->weapons)
        {
            const std::string vmPath = m_ctx->weapons->ActiveWeaponModel();
            Mesh* vm = vmPath.empty() ? nullptr : GetOrLoadEcsMesh(vmPath);
            if (vm && vm->GetVertexCount() > 0 && vm->GetIndexCount() > 0)
            {
                ID3D11DeviceContext* dc = gfx->GetContext();
                gfx->SetBasicShaders();
                const DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
                // Weapon OBJs are ~2.7 units long on their local X (barrel axis),
                // centered near (0.89,0.10,0). Recenter, scale to ~0.7 m, rotate
                // the barrel (local +X) to view forward (+Z), then place down-
                // right and far enough forward to clear the 0.5 m near plane.
                // view space: +x right, +y up, +z forward.
                const DirectX::XMMATRIX local =
                    DirectX::XMMatrixTranslation(-0.89f, -0.10f, 0.0f) *
                    DirectX::XMMatrixScaling(0.26f, 0.26f, 0.26f) *
                    DirectX::XMMatrixRotationY(-DirectX::XM_PIDIV2) *
                    DirectX::XMMatrixTranslation(0.28f, -0.26f, 0.9f);
                const DirectX::XMMATRIX vmWorld = local * invView;

                const DirectX::XMFLOAT4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
                const auto& subs = vm->GetSubmeshes();
                if (subs.empty())
                {
                    gfx->UpdateBasicConstants(vmWorld, view, proj, kWhite, {1.0f, 1.0f});
                    gfx->SetBasicTexture(nullptr);
                    vm->Render(dc);
                }
                else
                {
                    for (const MeshSubmesh& sm : subs)
                    {
                        ID3D11ShaderResourceView* srv =
                            sm.diffuseTexture.empty() ? nullptr : gfx->GetOrLoadTextureSRV(sm.diffuseTexture);
                        gfx->UpdateBasicConstants(vmWorld, view, proj, sm.diffuseColor, {1.0f, 1.0f});
                        gfx->SetBasicTexture(srv);
                        vm->RenderRange(dc, sm.indexStart, sm.indexCount);
                    }
                }
                gfx->SetBasicTexture(nullptr);
            }
        }
    }
    catch (const std::exception& e)
    {
        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "TFRender", 5,
                                "[TF] RenderWorld exception: %s", e.what());
    }
    catch (...)
    {
        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "TFRender", 5,
                                "[TF] RenderWorld: unknown exception");
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
    if (!nm.IsInitialized() && !nm.Initialize()) {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Connect: NetworkManager init failed");
        return false;
    }
    if (!nm.Connect(ip, port, "TerrafrontPlayer")) {
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
    (void)ip; (void)port;
    SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Connect: built without ENABLE_NETWORKING");
    return false;
#endif
}

#ifdef ENABLE_NETWORKING

bool TFWorldSetup::BootServer(uint16_t port, NetRole role)
{
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    if (!nm.IsInitialized() && !nm.Initialize()) {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] BootServer: NetworkManager init failed");
        return false;
    }
    if (!nm.StartServer(port, static_cast<int>(kMaxPlayers))) {
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
    if (!m_worldServer->Start(wc)) {
        SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] WorldServer failed to start (continuing: NetworkManager carries gameplay)");
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
    if (m_areaServer->Start(ac)) {
        if (m_ctx->serverSim)
            AttachSimulation(*m_areaServer, m_ctx->serverSim);
    } else {
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

    for (const auto& [clientId, info] : clients) {
        if (m_knownClients.insert(clientId).second)
            m_worldServer->HandlePlayerConnect(clientId, info.name, {0.0f, 0.0f, 0.0f});
    }
    for (auto it = m_knownClients.begin(); it != m_knownClients.end();) {
        if (clients.find(*it) == clients.end()) {
            m_worldServer->HandlePlayerDisconnect(*it);
            it = m_knownClients.erase(it);
        } else {
            ++it;
        }
    }
}

void TFWorldSetup::StopNetworking()
{
    if (m_areaServer) {
        m_areaServer->Stop();
        m_areaServer.reset();
    }
    if (m_worldServer) {
        m_worldServer->Stop();
        m_worldServer.reset();
    }
    if (m_netBooted) {
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
    if (m_netBooted) {
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

    MaybeStartAmbientAudio();
}

void TFWorldSetup::MaybeStartAmbientAudio()
{
    if (m_ambientStarted || !m_ctx->HasLocalPlayer() || !m_ctx->engine)
        return;
    ::AudioEngine* audio = m_ctx->engine->GetAudio();
    if (!audio)
        return; // no audio system (headless / init failed) — try again next frame? no: mark done
    m_ambientStarted = true;

    const char* kWind = "Audio/MMOFPS/ambient/wind_loop.wav";
    if (FAILED(audio->LoadSound(kWind, L"Assets/Audio/MMOFPS/ambient/wind_loop.wav")))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("[TFAudio] ambient wind load FAIL");
        return;
    }
    audio->PlaySound(kWind, 0.35f, 1.0f, /*loop*/ true);
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
    m_camera.reset();
    m_scene = nullptr;
    m_ownScene.reset();
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

#ifdef ENABLE_NETWORKING
    ImGui::Text("Net booted: %s | WorldServer: %s | AreaServer: %s", m_netBooted ? "yes" : "no",
                (m_worldServer && m_worldServer->IsRunning()) ? "up" : "down",
                (m_areaServer && m_areaServer->IsRunning()) ? "up" : "down");
    ImGui::Text("Sessions bridged: %zu", m_knownClients.size());
#endif

    if (m_origin) {
        const auto& stats = m_origin->GetStats();
        ImGui::Text("Origin rebases: %u (max dist %.0f m)", stats.totalRebases, stats.maxDistanceFromOrigin);
    }
#endif
}

} // namespace Terrafront
