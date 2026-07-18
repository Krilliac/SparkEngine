/**
 * @file TFMedalSystemClient.cpp
 * @brief TFMedalSystem client half: the TF_ScoreUpdate score-row mirror, the
 *        local-player medal toast queue, the self-registering NetworkManager
 *        handler lifecycle, and the toast overlay + debug UI rendering.
 *        Split from TFMedalSystem.cpp.
 */
#include "Game/TFMedalSystem.h"

#include "Graphics/GraphicsEngine.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        constexpr float kToastSec = 4.0f;     ///< medal toast lifetime
        constexpr float kToastFadeSec = 1.0f; ///< fade-out tail

    } // namespace

    // ---------------------------------------------------------------------------
    // Client: mirror + toasts
    // ---------------------------------------------------------------------------

    void TFMedalSystem::ClientHandleScore(const TF_ScoreUpdate& st)
    {
        TFScoreRow& row = m_mirror[st.player];
        row.score = st.score;
        row.kills = st.kills;
        row.deaths = st.deaths;
        row.captures = st.captures;
        row.medals = st.medals;
        row.faction =
            st.faction < static_cast<uint8_t>(FactionId::COUNT) ? static_cast<FactionId>(st.faction) : FactionId::None;
        row.cls = st.cls < static_cast<uint8_t>(ClassId::COUNT) ? static_cast<ClassId>(st.cls) : ClassId::COUNT;
    }

    void TFMedalSystem::ClientHandleMedal(const TF_MedalAward& award)
    {
        if (!m_ctx || !m_ctx->HasLocalPlayer() || award.player != m_ctx->localPlayer)
            return; // owner-only toast
        Toast t{};
        t.medal = award.medal;
        t.sessionCount = award.sessionCount;
        t.ttl = kToastSec;
        m_toasts.push_back(t);
        while (m_toasts.size() > 4) // never stack a wall of toasts
            m_toasts.pop_front();
    }

#ifdef ENABLE_NETWORKING

    bool TFMedalSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFMedalSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgScoreUpdate),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_ScoreUpdate))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_ScoreUpdate st{};
                               std::memcpy(&st, m.payload.data(), sizeof(st));
                               ClientHandleScore(st);
                           });
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgMedalAward),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_MedalAward))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_MedalAward award{};
                               std::memcpy(&award, m.payload.data(), sizeof(award));
                               ClientHandleMedal(award);
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] medal/score mirror handlers registered");
    }

    void TFMedalSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgScoreUpdate),
                           [](const Spark::Net::NetworkMessage&) {});
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgMedalAward),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFMedalSystem::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || m_toasts.empty())
            return;

        GraphicsEngine* gfx = m_ctx->engine ? m_ctx->engine->GetGraphics() : nullptr;
        const bool canDrawIcons = gfx && gfx->GetDevice() && gfx->GetContext();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float iconSize = 44.0f;
        const float rowH = iconSize + 10.0f;
        const float width = 320.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - width) * 0.5f, vp->Pos.y + vp->Size.y * 0.16f));
        ImGui::SetNextWindowSize(ImVec2(width, rowH * static_cast<float>(m_toasts.size()) + 16.0f));
        ImGui::SetNextWindowBgAlpha(0.30f); // faint plate so toasts read over combat
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (!ImGui::Begin("##TFMedalToasts", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        for (const Toast& t : m_toasts)
        {
            const TFMedalDef& def = TFMedalDefOf(t.medal);
            const float alpha = t.ttl < kToastFadeSec ? std::max(0.0f, t.ttl / kToastFadeSec) : 1.0f;

            ImGui::BeginGroup();
            bool drewIcon = false;
            if (canDrawIcons)
            {
                if (ID3D11ShaderResourceView* srv = gfx->GetOrLoadTextureSRV(TFMedalIconPath(t.medal, t.sessionCount)))
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
                    ImGui::Image(static_cast<void*>(srv), ImVec2(iconSize, iconSize));
                    ImGui::PopStyleVar();
                    drewIcon = true;
                }
            }
            if (!drewIcon)
                ImGui::Dummy(ImVec2(iconSize, iconSize)); // headless / missing texture
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, alpha), "%s", def.name);
            if (t.sessionCount > 1)
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, alpha), "+%u XP   (x%u)", def.xp, t.sessionCount);
            else
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, alpha), "+%u XP", def.xp);
            ImGui::EndGroup();
            ImGui::EndGroup();
            ImGui::Spacing();
        }

        ImGui::End();
    }

    void TFMedalSystem::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Medals"))
            return;
        ImGui::Text("server rows : %zu", m_server.size());
        ImGui::Text("mirror rows : %zu", m_mirror.size());
        ImGui::Text("awarded     : %u", m_medalsAwarded);
        ImGui::Text("rows sent   : %u", m_rowsSent);
        ImGui::Text("bad packets : %u", m_badPackets);
        ImGui::Text("toasts      : %zu", m_toasts.size());
        if (m_ctx && m_ctx->IsAuthority())
        {
            for (const auto& [player, rec] : m_server)
                ImGui::Text("  P%u  score %u  K%u/D%u  cap %u  medals %u  streak %u", player, rec.row.score,
                            rec.row.kills, rec.row.deaths, rec.row.captures, rec.row.medals, rec.streak);
        }
    }

#else // !SPARK_HAS_IMGUI — headless: detection + wire only

    void TFMedalSystem::RenderUI() {}
    void TFMedalSystem::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
