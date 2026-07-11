/**
 * @file TFPingUI.cpp
 * @brief Q-key ping input (tap = context, hold = wheel), distance labels,
 *        and the place/receive bleeps. See TFPingUI.h for the input model.
 */
#include "UI/TFPingUI.h"

#include "Game/TFAbilitySystem.h"
#include "Game/TFPingSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h"
#include "UI/TFChatWindow.h"
#include "UI/TFHUD.h"
#include "UI/TFLoginFlow.h"
#include "UI/TFMapScreen.h"
#include "UI/TFNameplates.h" // kTFNameplateAimConeCos (reused aim-cone helper constant)
#include "UI/TFScoreboard.h"
#include "UI/TFSocialPanel.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFWorldSetup.h"

#include "Audio/AudioEngine.h"
#include "Camera/SparkEngineCamera.h"
#include "Input/InputManager.h"
#include "Physics/PhysicsSystem.h" // engine umbrella header; stub-safe when Jolt is absent
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace Terrafront
{

    namespace
    {
        // Terrain ray-march (the heightfield is analytic — never in physics).
        constexpr float kTerrainMarchStartM = 2.0f;
        constexpr float kTerrainMarchStepM = 4.0f;
        constexpr int kTerrainBisectIters = 12;

        // Cone-target occlusion slack: the pawn may sit ON the world hit
        // (e.g. leaning out of cover) without being counted as behind it.
        constexpr float kConeOcclusionSlackM = 1.0f;

        constexpr const char* kPingBleep = "Audio/MMOFPS/ui/terminal_bleep_02.wav";

        /// Lazy-loading one-shot bleep (TFUiSounds_Play recipe; function-local
        /// static is one image-local copy — module-TU-only header rule).
        void PlayPingBleep(const TFGameContext* ctx, float vol, float pitch)
        {
            if (!ctx || !ctx->engine || !ctx->HasLocalPlayer())
                return;
            ::AudioEngine* audio = ctx->engine->GetAudio();
            if (!audio)
                return;
            static bool s_loaded = false;
            static bool s_ok = false;
            if (!s_loaded)
            {
                s_loaded = true;
                const std::string full = std::string("Assets/") + kPingBleep;
                s_ok = SUCCEEDED(audio->LoadSound(kPingBleep, std::wstring(full.begin(), full.end())));
                if (!s_ok)
                    SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] ping bleep load FAIL %s", kPingBleep);
            }
            if (s_ok)
                audio->PlaySound(kPingBleep, vol, pitch);
        }
    } // namespace

    TFPingUI::TFPingUI() = default;

    TFPingUI::~TFPingUI()
    {
        if (m_initialized)
            Shutdown();
    }

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    bool TFPingUI::Initialize(TFGameContext& ctx, TFEventBus& events, TFPingSystem& pings)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_pings = &pings;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFPingUI initialized");
        return true;
    }

    void TFPingUI::Shutdown()
    {
        if (!m_initialized)
            return;
        m_heard.clear();
        ResetInputState();
        m_initialized = false;
    }

    void TFPingUI::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // presentation-only; nothing simulates
    }

    void TFPingUI::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx || !m_pings || !m_ctx->HasLocalPlayer())
            return;

        SweepReceiveAudio();

        if (!m_ctx->InWorld() || m_ctx->localPlayer == kInvalidPlayer || FullscreenUiOpen() || !LocalPawnAlive())
        {
            ResetInputState();
            return;
        }
        PollPingKey(deltaTime);
    }

    void TFPingUI::ResetInputState()
    {
        m_armed = false;
        m_holdSec = 0.0f;
        m_wheelOpen = false;
    }

    // ---------------------------------------------------------------------------
    // Gates
    // ---------------------------------------------------------------------------

    bool TFPingUI::FullscreenUiOpen() const
    {
        // Same input-suppression gate set as TFGrenadeSystem::ClientPollThrowKey.
        return (m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
               (m_ctx->loginFlow && m_ctx->loginFlow->IsOpen()) || (m_ctx->hud && m_ctx->hud->IsChatOpen()) ||
               (m_ctx->chatWindow && m_ctx->chatWindow->IsOpen()) ||
               (m_ctx->socialPanel && m_ctx->socialPanel->IsOpen());
    }

    bool TFPingUI::LocalPawnAlive() const
    {
        PawnInfo pi{};
        return m_ctx->players && m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pi) && pi.alive;
    }

    // ---------------------------------------------------------------------------
    // Input: Q tap / hold + wheel selection
    // ---------------------------------------------------------------------------

    void TFPingUI::PollPingKey(float dt)
    {
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (!input)
            return;

        if (input->WasKeyPressed(m_pingVk))
        {
            m_armed = true;
            m_holdSec = 0.0f;
        }

        if (m_armed && input->IsKeyDown(m_pingVk))
        {
            m_holdSec += dt;
            if (!m_wheelOpen && m_holdSec >= kTFPingHoldSec)
                m_wheelOpen = true;
        }

        if (m_wheelOpen)
            PollWheelSelection();

        if (input->WasKeyReleased(m_pingVk))
        {
            if (m_armed && !m_wheelOpen)
                PlaceContextPing();
            // Releasing with the wheel open closes it (no ping without 1/2/3).
            ResetInputState();
        }
    }

    void TFPingUI::PollWheelSelection()
    {
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (!input)
            return;
        if (input->WasKeyPressed('1'))
        {
            PlaceWheelPing(TFPingType::Location);
            ResetInputState();
        }
        else if (input->WasKeyPressed('2'))
        {
            PlaceWheelPing(TFPingType::Enemy);
            ResetInputState();
        }
        else if (input->WasKeyPressed('3'))
        {
            PlaceWheelPing(TFPingType::NeedSupport);
            ResetInputState();
        }
    }

    // ---------------------------------------------------------------------------
    // Ray resolution (context ping + wheel Location/Enemy)
    // ---------------------------------------------------------------------------

    bool TFPingUI::ResolveWorldPoint(const float camPos[3], const float fwd[3], float outPos[3], float& outDist) const
    {
        float bestDist = kTFPingMaxRangeM + 1.0f;
        bool hit = false;

        // 1) Physics statics / vehicles / deployables (never pawns — the aim
        //    cone handles those; never the terrain — it is analytic).
        if (m_ctx->engine)
        {
            if (::PhysicsSystem* physics = m_ctx->engine->GetPhysics())
            {
                const DirectX::XMFLOAT3 o{camPos[0], camPos[1], camPos[2]};
                const DirectX::XMFLOAT3 d{fwd[0], fwd[1], fwd[2]};
                const RaycastHit rc = physics->RaycastFiltered(o, d, kTFPingMaxRangeM,
                                                               CollisionLayers::WorldStatic | CollisionLayers::Vehicle |
                                                                   CollisionLayers::Deployable);
                if (rc.hasHit && rc.distance <= kTFPingMaxRangeM)
                {
                    bestDist = rc.distance;
                    outPos[0] = rc.point.x;
                    outPos[1] = rc.point.y;
                    outPos[2] = rc.point.z;
                    hit = true;
                }
            }
        }

        // 2) Analytic terrain: march the ray, bisect the crossing step.
        if (m_ctx->world)
        {
            float prevT = kTerrainMarchStartM;
            const float endT = std::min(kTFPingMaxRangeM, bestDist);
            for (float t = kTerrainMarchStartM; t <= endT; t += kTerrainMarchStepM)
            {
                const float px = camPos[0] + fwd[0] * t;
                const float py = camPos[1] + fwd[1] * t;
                const float pz = camPos[2] + fwd[2] * t;
                if (py <= m_ctx->world->TerrainHeightAt(px, pz))
                {
                    float lo = prevT, hi = t;
                    for (int i = 0; i < kTerrainBisectIters; ++i)
                    {
                        const float mid = (lo + hi) * 0.5f;
                        const float mx = camPos[0] + fwd[0] * mid;
                        const float my = camPos[1] + fwd[1] * mid;
                        const float mz = camPos[2] + fwd[2] * mid;
                        if (my <= m_ctx->world->TerrainHeightAt(mx, mz))
                            hi = mid;
                        else
                            lo = mid;
                    }
                    const float tx = camPos[0] + fwd[0] * hi;
                    const float tz = camPos[2] + fwd[2] * hi;
                    bestDist = hi;
                    outPos[0] = tx;
                    outPos[1] = m_ctx->world->TerrainHeightAt(tx, tz);
                    outPos[2] = tz;
                    hit = true;
                    break;
                }
                prevT = t;
            }
        }

        outDist = bestDist;
        return hit;
    }

    EntityId TFPingUI::ResolveConeEnemy(const float camPos[3], const float fwd[3], float worldDist,
                                        float outPos[3]) const
    {
        if (!m_ctx->players)
            return 0;
        const FactionId myFaction = m_ctx->localFaction;
        const PlayerId local = m_ctx->localPlayer;

        EntityId best = 0;
        float bestDist = kTFPingMaxRangeM + 1.0f;
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.owner == local || p.faction == FactionId::None ||
                    (myFaction != FactionId::None && p.faction == myFaction))
                    return;
                // Cloak parity (TFNameplates): a veiled Ghost cannot be pinged.
                if (m_ctx->abilities && m_ctx->abilities->IsPawnVeiled(p.entity))
                    return;
                const float chest[3] = {p.pos[0], p.pos[1] + WeaponMath::kPawnHeightM * 0.5f, p.pos[2]};
                float to[3] = {chest[0] - camPos[0], chest[1] - camPos[1], chest[2] - camPos[2]};
                const float dist = WeaponMath::Len3(to);
                if (dist < 0.75f || dist > kTFPingMaxRangeM || dist >= bestDist)
                    return;
                if (dist > worldDist + kConeOcclusionSlackM)
                    return; // behind the world hit (wall/vehicle backdrop)
                if (!WeaponMath::Normalize3(to))
                    return;
                // Nameplate aim-cone helper constant (~2 deg around forward).
                if (WeaponMath::Dot3(to, fwd) < kTFNameplateAimConeCos)
                    return;
                best = p.entity;
                bestDist = dist;
                outPos[0] = p.pos[0];
                outPos[1] = p.pos[1];
                outPos[2] = p.pos[2];
            });
        return best;
    }

    void TFPingUI::PlaceContextPing()
    {
        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (!cam)
            return;
        const DirectX::XMFLOAT3 cp = cam->GetPosition();
        const DirectX::XMFLOAT3 cf = cam->GetForward();
        float fwd[3] = {cf.x, cf.y, cf.z};
        if (!WeaponMath::Normalize3(fwd))
            return;
        const float camPos[3] = {cp.x, cp.y, cp.z};

        float worldPos[3]{};
        float worldDist = kTFPingMaxRangeM + 1.0f;
        const bool haveWorld = ResolveWorldPoint(camPos, fwd, worldPos, worldDist);

        float enemyPos[3]{};
        const EntityId enemy = ResolveConeEnemy(camPos, fwd, worldDist, enemyPos);
        if (enemy != 0)
        {
            m_pings->UiRequestPing(TFPingType::Enemy, enemyPos, enemy);
            ++m_tapPings;
            PlayPingBleep(m_ctx, 0.42f, 1.10f);
            return;
        }
        if (haveWorld)
        {
            m_pings->UiRequestPing(TFPingType::Location, worldPos, 0);
            ++m_tapPings;
            PlayPingBleep(m_ctx, 0.42f, 1.10f);
            return;
        }
        ++m_noTarget; // open sky — nothing to anchor to
    }

    void TFPingUI::PlaceWheelPing(TFPingType type)
    {
        if (type == TFPingType::NeedSupport)
        {
            PawnInfo me{};
            if (!m_ctx->players || !m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, me) || !me.alive)
                return;
            m_pings->UiRequestPing(TFPingType::NeedSupport, me.pos, 0);
            ++m_wheelPings;
            PlayPingBleep(m_ctx, 0.42f, 1.10f);
            return;
        }

        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (!cam)
            return;
        const DirectX::XMFLOAT3 cp = cam->GetPosition();
        const DirectX::XMFLOAT3 cf = cam->GetForward();
        float fwd[3] = {cf.x, cf.y, cf.z};
        if (!WeaponMath::Normalize3(fwd))
            return;
        const float camPos[3] = {cp.x, cp.y, cp.z};

        float worldPos[3]{};
        float worldDist = kTFPingMaxRangeM + 1.0f;
        const bool haveWorld = ResolveWorldPoint(camPos, fwd, worldPos, worldDist);

        if (type == TFPingType::Enemy)
        {
            float enemyPos[3]{};
            const EntityId enemy = ResolveConeEnemy(camPos, fwd, worldDist, enemyPos);
            if (enemy == 0)
            {
                ++m_noTarget;
                PlayPingBleep(m_ctx, 0.30f, 0.75f); // soft deny
                return;
            }
            m_pings->UiRequestPing(TFPingType::Enemy, enemyPos, enemy);
        }
        else // Location
        {
            if (!haveWorld)
            {
                ++m_noTarget;
                PlayPingBleep(m_ctx, 0.30f, 0.75f); // soft deny
                return;
            }
            m_pings->UiRequestPing(TFPingType::Location, worldPos, 0);
        }
        ++m_wheelPings;
        PlayPingBleep(m_ctx, 0.42f, 1.10f);
    }

    // ---------------------------------------------------------------------------
    // Receive audio (mirror new-id sweep; role-agnostic)
    // ---------------------------------------------------------------------------

    void TFPingUI::SweepReceiveAudio()
    {
        std::unordered_set<uint16_t> liveNow;
        bool receiveBleep = false;
        const PlayerId local = m_ctx->localPlayer;
        m_pings->ForEachActivePing(
            [&](const TFPingView& v)
            {
                liveNow.insert(v.pingId);
                if (!m_heard.contains(v.pingId) && v.owner != local)
                    receiveBleep = true;
            });
        m_heard = std::move(liveNow); // heard set == live set (dedup + prune)
        if (receiveBleep)
            PlayPingBleep(m_ctx, 0.30f, 1.25f);
    }

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    namespace
    {
        /// World -> screen through the SAME matrices the frame was rendered
        /// with. MUST MATCH TFWorldSetup::ComputeViewProj's first-person branch
        /// (LookAtLH from the module camera pose; PerspectiveFovLH with
        /// XM_PIDIV4 * 1.6, window aspect, 0.3, 6000) — the TFNameplates
        /// ProjectToScreen mirror. If the render FOV or clip planes ever
        /// change, change this too or labels drift off the markers.
        bool PingProjectToScreen(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj, const ImGuiViewport& vp,
                                 const float world[3], ImVec2& out)
        {
            using namespace DirectX;
            const XMVECTOR ws = XMVectorSet(world[0], world[1], world[2], 1.0f);
            const XMVECTOR vs = XMVector3TransformCoord(ws, view);
            if (XMVectorGetZ(vs) < 0.3f)
                return false; // behind / inside the near plane (LH: +z forward)
            const XMVECTOR ndc = XMVector3TransformCoord(vs, proj);
            const float nx = XMVectorGetX(ndc);
            const float ny = XMVectorGetY(ndc);
            if (nx < -1.25f || nx > 1.25f || ny < -1.25f || ny > 1.25f)
                return false; // comfortably off-screen
            out.x = vp.Pos.x + (nx * 0.5f + 0.5f) * vp.Size.x;
            out.y = vp.Pos.y + (0.5f - ny * 0.5f) * vp.Size.y;
            return true;
        }

        ImU32 PingCol32(TFPingType t, float alpha)
        {
            float c[4];
            PingColor(t, c);
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], alpha));
        }
    } // namespace

    void TFPingUI::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_pings || !m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;
        if (FullscreenUiOpen())
            return;
        if (m_ctx->scoreboard && m_ctx->scoreboard->IsOpen())
            return;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (!vp || vp->Size.x <= 0.0f || vp->Size.y <= 0.0f)
            return;

        // ---- distance labels under the 3D markers ------------------------------
        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (cam && LocalPawnAlive())
        {
            using namespace DirectX;
            const XMFLOAT3 cp = cam->GetPosition();
            const XMFLOAT3 cf = cam->GetForward();
            float fwd[3] = {cf.x, cf.y, cf.z};
            if (WeaponMath::Normalize3(fwd))
            {
                const XMVECTOR eye = XMLoadFloat3(&cp);
                const XMVECTOR fv = XMVectorSet(fwd[0], fwd[1], fwd[2], 0.0f);
                const XMMATRIX view = XMMatrixLookAtLH(eye, XMVectorAdd(eye, fv), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
                const XMMATRIX proj =
                    XMMatrixPerspectiveFovLH(XM_PIDIV4 * 1.6f, vp->Size.x / vp->Size.y, 0.3f, 6000.0f);

                ImDrawList* dl = ImGui::GetForegroundDrawList();
                m_pings->ForEachActivePing(
                    [&](const TFPingView& p)
                    {
                        // Label sits just under the marker's bob midline.
                        const float anchor[3] = {p.pos[0], p.pos[1] + (p.type == TFPingType::Enemy ? 1.55f : 0.20f),
                                                 p.pos[2]};
                        ImVec2 sp;
                        if (!PingProjectToScreen(view, proj, *vp, anchor, sp))
                            return;
                        const float dx = p.pos[0] - cp.x, dy = p.pos[1] - cp.y, dz = p.pos[2] - cp.z;
                        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        const float alpha = std::clamp(p.lifeLeft / kTFPingMarkerFadeSec, 0.0f, 1.0f);
                        if (alpha <= 0.02f)
                            return;
                        char label[48];
                        std::snprintf(label, sizeof(label), "%s %.0fm", PingTypeName(p.type), dist);
                        const float fontSize = ImGui::GetFontSize() * 0.92f;
                        const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fontSize, 1.0e9f, 0.0f, label);
                        const ImVec2 pos(sp.x - sz.x * 0.5f, sp.y + 4.0f);
                        dl->AddText(ImGui::GetFont(), fontSize, ImVec2(pos.x + 1.0f, pos.y + 1.0f),
                                    IM_COL32(0, 0, 0, static_cast<int>(alpha * 190.0f)), label);
                        dl->AddText(ImGui::GetFont(), fontSize, pos, PingCol32(p.type, alpha), label);
                    });
            }
        }

        // ---- hold-wheel ---------------------------------------------------------
        if (m_wheelOpen)
        {
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.62f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.82f);
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
            if (ImGui::Begin("##TFPingWheel", nullptr, flags))
            {
                ImGui::TextDisabled("PING");
                ImGui::Separator();
                float c[4];
                PingColor(TFPingType::Location, c);
                ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "[1] Location");
                PingColor(TFPingType::Enemy, c);
                ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "[2] Enemy");
                PingColor(TFPingType::NeedSupport, c);
                ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "[3] Need Support");
                ImGui::Separator();
                ImGui::TextDisabled("release to cancel");
            }
            ImGui::End();
        }
    }

    void TFPingUI::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Ping UI"))
            return;
        ImGui::Text("tap pings   : %u", m_tapPings);
        ImGui::Text("wheel pings : %u", m_wheelPings);
        ImGui::Text("no target   : %u", m_noTarget);
        ImGui::Text("wheel open  : %s (held %.2fs)", m_wheelOpen ? "yes" : "no", m_holdSec);
    }

#else // !SPARK_HAS_IMGUI — headless: input/audio state only

    void TFPingUI::RenderUI() {}
    void TFPingUI::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
