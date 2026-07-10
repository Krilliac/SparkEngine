/**
 * @file TFWorldSetup.h
 * @brief Scene/terrain load, WorldServer/AreaServer boot, origin-rebase driving.
 *
 * OWNERSHIP: this header + TFWorldSetup.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W1: loads Assets/Scenes/MMOFPS/cindral_wastes.scene, owns the procedural
 * Cindral Wastes heightfield (TerrainHeightAt), boots NetworkManager +
 * WorldServer + the single continent AreaServer per NetRole, and drives
 * WorldOriginSystem::Update from the local pawn position on clients.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace Spark::World
{
    class WorldOriginSystem;
}
#ifdef ENABLE_NETWORKING
namespace Spark::Net
{
    class WorldServer;
    class AreaServer;
} // namespace Spark::Net
#endif
class SparkEngineCamera;
class SceneManager;
class Mesh;

namespace Terrafront
{

    struct WorldPresentationDef; // Data/TFDataTables.h (full def in the .cpp)
    class TFWorldCollision;      // World/TFWorldCollision.h (static scene bodies + move resolver)

    /// Procedural heightfield parameters for the Cindral Wastes continent.
    /// Defaults MUST equal the tf* keys authored in cindral_wastes.scene
    /// [Terrain]; the scene values override these at load so the authored scene
    /// and the runtime height function can never drift apart.
    struct TFTerrainParams
    {
        float baseHeight = 8.0f;
        float duneAmp = 4.0f;
        float dunePeriodX = 0.0040f;
        float dunePeriodZ = 0.0035f;
        float ridgeAmp = 2.0f;
        // canyon separating the SW (AUC) and SE (HLX) quadrants
        float canyonX = 2048.0f;
        float canyonHalfW = 260.0f;
        float canyonZ0 = 250.0f;
        float canyonZ1 = 1550.0f;
        float canyonDepth = 16.0f;
        // flat build plateau blended in around every region center
        float plateauRadius = 120.0f;
        float plateauSkirt = 180.0f;
        float plateauSky = 40.0f;
        float plateauFort = 30.0f;
        float plateauFacility = 26.0f;
        float plateauOutpost = 20.0f;
    };

    class TFWorldSetup
    {
      public:
        TFWorldSetup();
        ~TFWorldSetup();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- frozen cross-system API (DESIGN.md, W1) ---

        /// Boot as in-process server with a local player (ListenHost role).
        bool StartHost(uint16_t port);

        /// Boot as headless authoritative server (DedicatedServer role).
        bool StartDedicated(uint16_t port);

        /// Boot NetworkManager in client role and connect to a remote host.
        bool Connect(const std::string& ip, uint16_t port);

        /// Authoritative terrain height at world XZ. Deterministic and identical
        /// on server and client (same params, same region table). Safe to call
        /// from the AreaServer tick thread: reads only load-time-immutable state.
        float TerrainHeightAt(float x, float z) const;

        /// Render the loaded scene + all ECS visuals (pawns, vehicles,
        /// deployables) for this frame. Called from TerrafrontModule::OnRender —
        /// with a module loaded the module owns the frame, so this is the only
        /// BeginFrame/EndFrame in the process. No-op when headless (no graphics).
        ///
        /// Implementation note: all draw work goes through MEMBER functions of
        /// engine objects reached via the virtual IEngineContext getters. The
        /// module DLL statically links SparkEngineLib, so DLL-side globals like
        /// EngineContext::Get() are unset copies — GameObject::Render() (which
        /// reads that global) must NOT be called from module code.
        void RenderWorld();

        /// Module-owned first-person camera. The engine's EngineContext camera
        /// slot is empty in module mode (and the service-locator type ids are
        /// per-image, so a DLL-side registration would be invisible anyway);
        /// TFClientNet drives this camera instead. Null when headless.
        SparkEngineCamera* GetCamera() const { return m_camera.get(); }

        /// Spawn a short-lived muzzle flash + tracer from the fire origin along
        /// dir (called by TFWeaponSystem::ClientTriggerFire). Drawn in RenderWorld.
        void SpawnMuzzleFx(const float origin[3], const float dir[3]);

        /// Post-TFMoveStep world-collision resolve — THE shared client/server hook
        /// (2026-07-10 collision wave). Call immediately after TFMoveStep on BOTH
        /// the authoritative integrator (TFServerSim::StepPlayer) and the client
        /// prediction simulator (TFClientNet::SimulateMove) with the pre-step feet
        /// position; pos/vel are the TFMoveStep outputs, adjusted in place.
        /// Capsule-sweeps the horizontal move against the static scene bodies
        /// (TFWorldCollision, Movement mask) and slides along blockers, then
        /// re-clamps Y to TerrainHeightAt at the resolved column so walking below
        /// terrain is impossible regardless of physics availability. Deterministic
        /// (static bodies from the same scene file on both roles); a no-op beyond
        /// the terrain clamp when Jolt is absent.
        void ResolveMoveCollision(const float prevPos[3], float pos[3], float vel[3]) const;

        /// Static scene collision (null before Initialize; inactive without Jolt).
        TFWorldCollision* Collision() const { return m_collision.get(); }

      private:
        void LoadSceneAndTerrain();
        void ParseTerrainParams(const std::string& scenePath);
        float PlateauHeight(const std::string& tier) const;
        /// Data-driven presentation constants (skybox/terrain/ambient/viewmodel/
        /// muzzle-FX/pawn mesh). Returns ctx->data->GetPresentation() when loaded,
        /// else a static default-constructed WorldPresentationDef{} whose
        /// in-class member-initializers equal today's former hardcoded magic
        /// numbers — so behavior is byte-identical whether or not data is loaded.
        const WorldPresentationDef& Pres() const;
        void DriveOriginRebase();
        void CreateCamera();
        void ComputeViewProj(DirectX::XMMATRIX& outView, DirectX::XMMATRIX& outProj) const;
#ifdef ENABLE_NETWORKING
        bool BootServer(uint16_t port, NetRole role);
        void BridgeWorldServerSessions();
        void StopNetworking();
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        std::string m_scenePath; // repo-relative, e.g. Assets/Scenes/MMOFPS/cindral_wastes.scene
        bool m_sceneLoaded{false};
        TFTerrainParams m_terrain;

        // Static Jolt collision for the authored scene (built on BOTH roles right
        // after scene load; terrain itself stays analytic — see TFWorldCollision.h).
        std::unique_ptr<TFWorldCollision> m_collision;

        std::unique_ptr<Spark::World::WorldOriginSystem> m_origin;

        // --- rendering (client-side only; null when headless) ---
        std::unique_ptr<SparkEngineCamera> m_camera;
        std::unique_ptr<SceneManager> m_ownScene; // module-created when the engine has none
        SceneManager* m_scene{nullptr};           // whichever manager holds the scene

        // Device-direct mesh cache for ECS visuals (pawns/vehicles/deployables).
        // The engine's SubmitMeshForRendering/ProcessDrawList path loads meshes
        // through the AssetPipeline, whose OBJ loader is unreliable on Windows, so
        // pawns never drew. We load each unique mesh once via the same
        // tinyobjloader path the scene uses and draw it manually in RenderWorld.
        std::unordered_map<std::string, std::unique_ptr<Mesh>> m_ecsMeshCache;
        Mesh* GetOrLoadEcsMesh(const std::string& meshPath);

        // Short-lived shot effects (muzzle flash + tracer). Drawn as bright unlit
        // stretched cubes; expire by age against m_fxClock (advanced in Update).
        struct ShotFx
        {
            double t0;
            DirectX::XMFLOAT3 origin;
            DirectX::XMFLOAT3 dir;
        };
        std::vector<ShotFx> m_shotFx;
        double m_fxClock{0.0};
        std::unique_ptr<Mesh> m_fxCube; // unit cube reused for flash + tracer

        // Skybox: an inward-facing textured box centered on the camera, drawn
        // first so it sits behind the world. 6 face-quads in one mesh, each a
        // contiguous index range textured with one space_*.png cubemap face.
        std::unique_ptr<Mesh> m_skyMesh;
        void DrawSkybox(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

        // Procedural terrain relief: a heightfield mesh sampled from TerrainHeightAt
        // over the whole map, drawn instead of the flat scene ground plane.
        std::unique_ptr<Mesh> m_terrainMesh;
        void DrawTerrain(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

#ifdef ENABLE_NETWORKING
        std::unique_ptr<Spark::Net::WorldServer> m_worldServer;
        std::unique_ptr<Spark::Net::AreaServer> m_areaServer;
        std::unordered_set<uint32_t> m_knownClients; // Spark::Net::ClientID
#endif
        bool m_netBooted{false};      // this module booted NetworkManager (so it pumps + tears down)
        bool m_ambientStarted{false}; // looping ambience kicked off once a local player exists
        void MaybeStartAmbientAudio();
    };

} // namespace Terrafront
