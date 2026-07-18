/**
 * @file TFClientNetView.cpp
 * @brief TFClientNet presentation plumbing: remote-pawn interpolation, the
 *        first-person camera and the debug panel (same class, split per repo
 *        file-size rules — core pump/prediction logic lives in TFClientNet.cpp,
 *        the TFMsg handlers in TFClientNetHandlers.cpp and the loopback/local
 *        feedback path in TFClientNetLoopback.cpp).
 */
#include "Net/TFClientNet.h"

#include "Core/TFTypes.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h"
#include "Net/TFReplication.h"
#include "World/TFWorldSetup.h"

#include "Camera/SparkEngineCamera.h"
#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace Terrafront
{

    namespace
    {

        constexpr float kInterpDelaySec = 0.100f; // remote pawn render delay
        constexpr float kRadToDeg = 57.2957795f;
        constexpr double kTwoPi = 6.28318530717958647692;

        /// Shift `yaw` by whole turns so it lands nearest to `reference` (keeps the
        /// interpolation buffer free of ±pi wrap pops).
        float UnwrapNear(float reference, float yaw)
        {
            const double turns = std::round((static_cast<double>(reference) - yaw) / kTwoPi);
            return yaw + static_cast<float>(turns * kTwoPi);
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Remote pawn interpolation (pure client)
    // ---------------------------------------------------------------------------

    void TFClientNet::UpdateRemotePawns()
    {
        if (!m_ctx->replication || !m_ctx->players)
            return;

        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        const float renderTime = static_cast<float>(m_clock) - kInterpDelaySec;

        std::unordered_map<EntityId, InterpEntry>& interp = m_interp;
        std::vector<EntityId> seen;
        seen.reserve(interp.size() + 4);

        m_ctx->replication->ForEachRemotePawn(
            [&](const RemotePawn& rp)
            {
                seen.push_back(rp.entity);
                if (rp.owner == m_ctx->localPlayer)
                    return; // own pawn: first-person, prediction drives the camera

                InterpEntry& e = interp[rp.entity];
                if (!e.has || rp.recvTime != e.lastRecvTime)
                {
                    // New replication sample: timestamp on OUR clock so evaluation and
                    // sampling share one time base regardless of sender cadence.
                    const float y = e.has ? UnwrapNear(e.lastYaw, rp.yaw) : rp.yaw;
                    e.pos.AddSample({rp.pos[0], rp.pos[1], rp.pos[2]}, static_cast<float>(m_clock));
                    e.yaw.AddSample(y, static_cast<float>(m_clock));
                    e.lastYaw = y;
                    e.lastRecvTime = rp.recvTime;
                    e.has = true;
                }

                if (!world || !e.pos.HasSamples())
                    return;
                uint32_t localEnt = 0;
                if (!m_ctx->players->ResolveEntity(rp.entity, localEnt))
                    return; // visual entity not created yet (TFPlayerSystem::SyncClientRecords)
                const auto ecsEnt = static_cast<EntityID>(localEnt);
                if (!world->GetRegistry().valid(ecsEnt))
                    return;
                if (Transform* t = world->GetComponent<Transform>(ecsEnt))
                {
                    const DirectX::XMFLOAT3 p = e.pos.Evaluate(renderTime);
                    t->position = p;
                    t->rotation.y = e.yaw.Evaluate(renderTime) * kRadToDeg;
                }
            });

        // Drop buffers for entities that left the replication store.
        for (auto it = interp.begin(); it != interp.end();)
        {
            if (std::find(seen.begin(), seen.end(), it->first) == seen.end())
                it = interp.erase(it);
            else
                ++it;
        }
    }

    // ---------------------------------------------------------------------------
    // First-person camera
    // ---------------------------------------------------------------------------

    void TFClientNet::DriveFirstPersonCamera(bool aliveLocalPawn, const float feetPos[3])
    {
        if (!aliveLocalPawn)
            return;
        // Module-owned camera (TFWorldSetup) — the engine context camera slot is
        // never populated in module mode.
        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (!cam)
            return;

        float eye[3] = {feetPos[0], feetPos[1], feetPos[2]};
        if (!m_ctx->IsAuthority() && m_predActive)
        {
            eye[0] = m_predState.position.x;
            eye[1] = m_predState.position.y;
            eye[2] = m_predState.position.z;
        }
        // Authority roles read the pawn Transform truth (TFServerSim writes it).

        cam->SetPosition({eye[0], eye[1] + WeaponMath::kEyeHeightM, eye[2]});
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFClientNet::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!m_showDebug)
            return;
        if (ImGui::Begin("TF Client Net", &m_showDebug))
        {
            const char* mode = m_connected ? "remote client" : (LocalLoopback() ? "local loopback" : "idle");
            ImGui::Text("mode        : %s", mode);
            ImGui::Text("local player: 0x%08X", m_localPlayer);
            ImGui::Text("inputs sent : %u (seq %u)", m_inputsSent,
                        m_ctx && !m_ctx->IsAuthority() ? m_prediction.GetCurrentSequence() : m_inputSeq);
            ImGui::Text("view        : yaw %.2f pitch %.2f rad", m_viewYaw, m_viewPitch);
            if (m_ctx && !m_ctx->IsAuthority())
            {
                ImGui::Separator();
                ImGui::Text("prediction  : %s, pending %zu, correction %.3fm, reconciles %u",
                            m_predActive ? "active" : "off", m_prediction.GetPendingInputCount(),
                            m_prediction.GetLastCorrectionMagnitude(), m_reconciles);
                ImGui::Text("pred pos    : (%.1f %.1f %.1f) %s", m_predState.position.x, m_predState.position.y,
                            m_predState.position.z, m_predState.isGrounded ? "ground" : "air");
                ImGui::Text("interp pawns: %zu", m_interp.size());
            }
            ImGui::Text("rank        : %u (xp %u)", m_lastRank, m_lastXPTotal);
        }
        ImGui::End();
#endif
    }

} // namespace Terrafront
