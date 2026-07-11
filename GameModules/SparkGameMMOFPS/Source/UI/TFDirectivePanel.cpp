/**
 * @file TFDirectivePanel.cpp
 * @brief Directives panel implementation (see TFDirectivePanel.h).
 *
 * Roles:
 *  - authority (listen host / standalone): rows read TFDirectiveSystem live.
 *  - dedicated server: no UI, but Update() serves 0x544C snapshot requests.
 *  - pure client: 1 Hz 0x544C requests while open; rows read the last
 *    0x544D snapshot (server truth, never predicted).
 */
#include "UI/TFDirectivePanel.h"

#include "Game/TFDirectiveSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "UI/TFKeybinds.h" // ui-map-keys seam: Action::ToggleDirectives / CloseOverlay
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        /// kTFDirectives index of a stable directive id (-1 if unknown — e.g. a
        /// newer server's additive table entry this client build doesn't know).
        int DirectiveIndexOfId(uint16_t directiveId)
        {
            for (size_t i = 0; i < kTFDirectiveCount; ++i)
                if (kTFDirectives[i].id == directiveId)
                    return static_cast<int>(i);
            return -1;
        }

    } // namespace

    TFDirectivePanel::TFDirectivePanel() = default;
    TFDirectivePanel::~TFDirectivePanel()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFDirectivePanel::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDirectivePanel initialized (%zu directives)",
                       kTFDirectiveCount);
        return true;
    }

    void TFDirectivePanel::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        m_remoteAge += deltaTime;

#ifdef ENABLE_NETWORKING
        // Server side: serve snapshot requests while hosting (TFVehicleNet
        // lazy-handler pattern; also covers the dedicated server, which runs
        // Update() but never RenderUI()).
        if (ServerNetActive())
        {
            if (!m_serverHandlers)
                ServerEnsureHandlers();
        }
        else if (m_serverHandlers)
        {
            ServerReleaseHandlers();
        }

        // Client side: mirror handlers while a client link is up.
        if (ClientNetActive())
        {
            if (!m_clientHandlers)
                ClientEnsureHandlers();
            ClientPumpRequests(deltaTime);
        }
        else if (m_clientHandlers)
        {
            ClientReleaseHandlers();
            m_hasRemote = false;
        }
#else
        (void)deltaTime;
#endif
    }

    void TFDirectivePanel::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFDirectivePanel::Shutdown()
    {
        if (!m_initialized)
            return;
#ifdef ENABLE_NETWORKING
        if (m_serverHandlers)
            ServerReleaseHandlers();
        if (m_clientHandlers)
            ClientReleaseHandlers();
#endif
        m_open = false;
        m_releasedMouse = false;
        m_hasRemote = false;
        m_initialized = false;
    }

    bool TFDirectivePanel::LocalPawnAlive() const
    {
        if (!m_ctx || !m_ctx->players || m_ctx->localPlayer == kInvalidPlayer)
            return false;
        PawnInfo pawn{};
        return m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) && pawn.alive;
    }

    void TFDirectivePanel::Toggle()
    {
        if (m_open)
        {
            Close();
            return;
        }
        m_open = true;
        m_requestAccum = kTFDirectiveRequestSec; // pure client: request on the next Update
        // TFChatWindow pattern: the panel owns the mouse while open.
        if (m_ctx && m_ctx->engine && m_ctx->engine->GetInput())
        {
            InputManager* input = m_ctx->engine->GetInput();
            m_releasedMouse = input->IsMouseCaptured();
            if (m_releasedMouse)
                input->CaptureMouse(false);
        }
    }

    void TFDirectivePanel::Close()
    {
        m_open = false;
        if (m_releasedMouse && m_ctx && m_ctx->engine && m_ctx->engine->GetInput() && LocalPawnAlive())
            m_ctx->engine->GetInput()->CaptureMouse(true);
        m_releasedMouse = false;
    }

    // ---------------------------------------------------------------------------
    // Rows (shared shape for both data sources)
    // ---------------------------------------------------------------------------

    void TFDirectivePanel::BuildRows()
    {
        // Baseline: full table at zero (unknown player == zeros, matching
        // TFDirectiveSystem::ForEachStatus semantics).
        for (size_t i = 0; i < kTFDirectiveCount; ++i)
        {
            m_rows[i].def = &kTFDirectives[i];
            m_rows[i].progress = 0;
            m_rows[i].tiersDone = 0;
        }

        if (m_ctx->IsAuthority())
        {
            if (!m_ctx->directives || m_ctx->localPlayer == kInvalidPlayer)
                return;
            size_t i = 0;
            m_ctx->directives->ForEachStatus(m_ctx->localPlayer,
                                             [this, &i](const TFDirectiveSystem::Status& s)
                                             {
                                                 if (i >= m_rows.size())
                                                     return; // table grew mid-frame — impossible, but stay safe
                                                 m_rows[i].def = s.def;
                                                 m_rows[i].progress = s.progress;
                                                 m_rows[i].tiersDone = s.tiersDone;
                                                 ++i;
                                             });
            return;
        }

        if (!m_hasRemote)
            return;
        const size_t count = std::min<size_t>(m_remote.count, kTFDirectiveStatusMax);
        for (size_t e = 0; e < count; ++e)
        {
            const TF_DirectiveStatusEntry& entry = m_remote.entries[e];
            const int idx = DirectiveIndexOfId(entry.directiveId);
            if (idx < 0)
                continue; // newer server table entry — ignore (additive contract)
            m_rows[static_cast<size_t>(idx)].progress = entry.progress;
            m_rows[static_cast<size_t>(idx)].tiersDone =
                std::min<uint8_t>(entry.tiersDone, static_cast<uint8_t>(kTFDirectiveTiers));
        }
    }

    // ---------------------------------------------------------------------------
    // Networking (reserved block 0x544C-0x544F; TFVehicleNet handler pattern)
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFDirectivePanel::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && m_ctx->IsAuthority();
    }

    bool TFDirectivePanel::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Client;
    }

    void TFDirectivePanel::ServerEnsureHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsg_DirectiveStatusReq), [this](const NetworkMessage& m)
                           { ServerOnStatusRequest(m.senderID, m.payload.data(), m.payload.size()); });
        m_serverHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] directive panel server net handler registered");
    }

    void TFDirectivePanel::ServerReleaseHandlers()
    {
        // No per-type removal in NetworkManager; overwrite with a no-op so no
        // dangling `this` survives shutdown (module-wide pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsg_DirectiveStatusReq),
                           [](const Spark::Net::NetworkMessage&) {});
        m_serverHandlers = false;
    }

    void TFDirectivePanel::ClientEnsureHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsg_DirectiveStatus),
                           [this](const NetworkMessage& m) { ClientOnStatus(m.payload.data(), m.payload.size()); });
        m_clientHandlers = true;
    }

    void TFDirectivePanel::ClientReleaseHandlers()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsg_DirectiveStatus),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

    void TFDirectivePanel::ServerOnStatusRequest(PlayerId sender, const void* data, size_t size)
    {
        (void)data;
        // Empty-payload request (CharListRequest precedent).
        if (size != 0 || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        // Enter-world gate, same bar the TFMsg routing holds gameplay traffic to.
        if (!m_ctx->directives || !m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(sender))
            return;

        TF_DirectiveStatus rep{};
        size_t i = 0;
        m_ctx->directives->ForEachStatus(sender,
                                         [&rep, &i](const TFDirectiveSystem::Status& s)
                                         {
                                             if (i >= kTFDirectiveStatusMax)
                                                 return;
                                             rep.entries[i].directiveId = s.def->id;
                                             rep.entries[i].tiersDone = s.tiersDone;
                                             rep.entries[i].progress = s.progress;
                                             ++i;
                                         });
        rep.count = static_cast<uint8_t>(i);

        auto& nm = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(kTFMsg_DirectiveStatus);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(sizeof(rep));
        std::memcpy(msg.payload.data(), &rep, sizeof(rep));
        nm.SendToClient(sender, msg);
        ++m_requestsServed;
    }

    void TFDirectivePanel::ClientOnStatus(const void* data, size_t size)
    {
        if (size != sizeof(TF_DirectiveStatus))
        {
            ++m_badPackets;
            return;
        }
        std::memcpy(&m_remote, data, sizeof(m_remote));
        if (m_remote.count > kTFDirectiveStatusMax)
            m_remote.count = kTFDirectiveStatusMax;
        m_hasRemote = true;
        m_remoteAge = 0.0f;
        ++m_snapshotsApplied;
    }

    void TFDirectivePanel::ClientPumpRequests(float dt)
    {
        // Only pure clients poll — the authority reads TFDirectiveSystem direct.
        if (!m_open || m_ctx->IsAuthority() || !m_ctx->clientNet || !m_ctx->clientNet->IsConnected() ||
            !m_ctx->InWorld())
            return;
        m_requestAccum += dt;
        if (m_requestAccum < kTFDirectiveRequestSec)
            return;
        m_requestAccum = 0.0f;
        m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsg_DirectiveStatusReq), nullptr, 0);
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFDirectivePanel::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;

        // Toggle key (ui-map-keys TFKeybinds seam, default J). ImGui route so
        // typing in chat never toggles the panel (TFChatWindow guard set).
        const ImGuiKey toggleKey = TFKeys::ImGuiKeyFor(TFKeys::Action::ToggleDirectives);
        const ImGuiKey closeKey = TFKeys::ImGuiKeyFor(TFKeys::Action::CloseOverlay);
        const bool fullscreenBusy =
            (m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen());
        if (toggleKey != ImGuiKey_None && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(toggleKey, false) &&
            (m_open || (!fullscreenBusy && !ImGui::GetIO().WantCaptureKeyboard)))
            Toggle();
        if (m_open && closeKey != ImGuiKey_None && ImGui::IsKeyPressed(closeKey, false))
            Close();
        if (!m_open)
            return;
        if (fullscreenBusy)
        {
            Close(); // the map/spawn screens own the whole screen — yield
            return;
        }

        BuildRows();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float w = std::min(560.0f, vp->Size.x * 0.9f);
        const float h = std::min(480.0f, vp->Size.y * 0.85f);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + (vp->Size.y - h) * 0.5f),
                                ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Appearing);

        bool open = true;
        if (!ImGui::Begin("Directives##tf_directives", &open, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            if (!open)
                Close();
            return;
        }

        // Freshness line: authority is always live; pure clients show sync state.
        if (m_ctx->IsAuthority())
            ImGui::TextDisabled("Live server data. Tier rewards pay out automatically on completion.");
        else if (!m_hasRemote)
            ImGui::TextDisabled("Syncing with server...");
        else
            ImGui::TextDisabled("Server snapshot %.0fs ago. Tier rewards pay out automatically on completion.",
                                m_remoteAge);
        ImGui::Separator();

        ImGui::BeginChild("##tf_directive_rows", ImVec2(0.0f, 0.0f), false);
        for (const Row& row : m_rows)
        {
            const TFDirectiveDef& def = *row.def;
            const bool complete = row.tiersDone >= kTFDirectiveTiers;

            ImGui::PushID(def.id);

            // Title line: name + tier tally.
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  complete ? IM_COL32(140, 235, 160, 255) : IM_COL32(235, 235, 235, 255));
            ImGui::TextUnformatted(def.name);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", def.desc);
            ImGui::SameLine();
            ImGui::TextDisabled("(tier %u/%u)", static_cast<unsigned>(row.tiersDone),
                                static_cast<unsigned>(kTFDirectiveTiers));

            // Progress bar toward the next tier goal (full when complete).
            char overlay[48];
            if (complete)
            {
                std::snprintf(overlay, sizeof(overlay), "%u — COMPLETE", row.progress);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(90, 190, 110, 255));
                ImGui::ProgressBar(1.0f, ImVec2(-1.0f, 0.0f), overlay);
                ImGui::PopStyleColor();
            }
            else
            {
                const uint32_t goal = def.tiers[row.tiersDone].goal;
                const float frac = goal > 0 ? std::min(1.0f, static_cast<float>(row.progress) / goal) : 0.0f;
                std::snprintf(overlay, sizeof(overlay), "%u / %u", row.progress, goal);
                ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
            }

            // Tier chips: goal + rewards, PAID/next/locked coloring.
            for (size_t t = 0; t < kTFDirectiveTiers; ++t)
            {
                const TFDirectiveTier& tier = def.tiers[t];
                const bool paid = row.tiersDone > t;
                const bool active = !paid && row.tiersDone == t;
                char chip[64];
                std::snprintf(chip, sizeof(chip), "%s%u: %u  +%uXP +%uF%s", paid ? "" : (active ? "> " : ""),
                              static_cast<unsigned>(t) + 1u, tier.goal, tier.xp, tier.flux, paid ? "  PAID" : "");
                const ImU32 col = paid     ? IM_COL32(140, 235, 160, 255)
                                  : active ? IM_COL32(230, 200, 110, 255)
                                           : IM_COL32(140, 140, 140, 255);
                if (t > 0)
                    ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(chip);
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::End();
        if (!open)
            Close();
    }

    void TFDirectivePanel::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Directive Panel"))
            return;
        ImGui::Text("open: %d  remote: %d (age %.1fs)", m_open ? 1 : 0, m_hasRemote ? 1 : 0, m_remoteAge);
        ImGui::Text("served: %u  applied: %u  bad packets: %u", m_requestsServed, m_snapshotsApplied, m_badPackets);
    }

#else // !SPARK_HAS_IMGUI

    void TFDirectivePanel::RenderUI() {}
    void TFDirectivePanel::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
