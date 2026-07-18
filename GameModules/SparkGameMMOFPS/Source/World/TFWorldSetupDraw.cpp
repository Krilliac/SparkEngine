/**
 * @file TFWorldSetupDraw.cpp
 * @brief TFWorldSetup camera + device-direct draw helpers: module camera,
 *        view/proj construction, the ECS mesh cache, and the skybox /
 *        procedural-terrain / scene-object draw paths. Frame composition
 *        (RenderWorld) lives in TFWorldSetupRender.cpp; scene/terrain load
 *        lives in TFWorldSetup.cpp (same class, split per the repo
 *        file-size rules — mirrors the TFRegionSystem/-Net split).
 */
#include "World/TFWorldSetup.h"

#include "World/TFSanctuaryZone.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"

#include "Spark/IEngineContext.h"
#include "SceneManager/SceneManager.h"
#include "Camera/SparkEngineCamera.h"
#include "Game/GameObject.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Game/PlaceholderMesh.h"
#include "Game/TFOpticsSystem.h" // W11 weapon-optics: scoped FOV zoom + hold-breath sway
#include "World/TFWeatherFx.h"   // W12 weather-visuals: storm cycle + client visuals
#include "Utils/LogMacros.h"

#include <cmath>
#include <string>
#include <vector>

namespace Terrafront
{

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

} // namespace Terrafront
