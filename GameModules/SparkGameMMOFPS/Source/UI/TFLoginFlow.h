/**
 * @file TFLoginFlow.h
 * @brief W5 onboarding: client ImGui state machine for login -> character
 *        select/create -> enter-world (TERRAFRONT, Task 5).
 *
 * OWNERSHIP: this header + TFLoginFlow.cpp belong to ONE implementation agent.
 * Styled like TFSpawnScreen (full-viewport NoDecoration|NoBackground modal,
 * dimmed backdrop, centered panel, TFUi helpers). Ported/adapted from
 * SparkGameMMO's MMOLoginUI, retargeted at the TFMsg onboarding protocol
 * (Net/TFNetProtocol.h, Task 4) and TERRAFRONT factions (MRA/AUC/HLX).
 *
 * Client-side only. Drives the flow by SENDING login/register/char requests
 * via `m_ctx->clientNet->SendMsg(...)`. Task 6 wired TFClientNet's onboarding
 * handlers (TFClientNetHandlers.cpp OnLoginReply/OnRegisterReply/
 * OnCharListReply/OnCharCreateReply/OnCharDeleteReply/OnWorldWelcome) to call
 * the reply sinks below (OnLoginReply/OnRegisterReply/OnCharList/
 * OnCharOpReply/OnEnteredWorld) directly via `m_ctx->loginFlow`, superseding
 * the pre-Task-6 getter-poll fallback (removed — see git history for
 * TFClientNet's IsLoggedIn/AccountId/LastAuthError/... getters, kept as a
 * read-only status surface but no longer polled here).
 *
 * NOTE: context wiring (the loginFlow pointer in TFGameContext, gating the
 * spawn screen behind InWorld, boot construction in Main.cpp) is Task 6 —
 * done; see DESIGN.md "W5 — Onboarding".
 *
 * W11 server-browser lane: TFLoginFlow additionally OWNS the TFLanDiscovery
 * instance (Game/TFLanDiscovery.h) — Main.cpp calls m_loginFlow->Update on
 * every role (dedicated servers included), so this one member drives BOTH the
 * server-side LAN beacon (self-arming off ctx.role after tf_host/tf_dedicated)
 * and the client-side scanner (armed only while the login screen renders, so
 * headless test runs never bind UDP 27025). The login screen gains a 'LAN
 * SERVERS' list whose Join button runs the existing tf_connect path
 * (TFWorldSetup::Connect) against the beacon's source IP + advertised port.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Game/TFLanDiscovery.h"
#include "Net/TFNetProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Terrafront
{

    enum class TFFlowState : uint8_t
    {
        Login,
        Register,
        CharSelect,
        CharCreate,
        EnteringWorld,
        InWorld,
    };

    class TFLoginFlow
    {
      public:
        TFLoginFlow();
        ~TFLoginFlow();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI();

        bool ToggleDebugUI() { return m_showDebug = !m_showDebug; }

        // --- W5 cross-agent contract (Task 6 gates the rest of the game UI on
        // this) ---
        bool IsOpen() const { return m_state != TFFlowState::InWorld; }
        TFFlowState State() const { return m_state; }

        /// multimap-plumbing lane (W13) gap #3 fix: nothing previously reset
        /// `m_state` back to Login on disconnect, so a manual tf_disconnect or
        /// a continent hop (TFTravelSystem::ClientRequestContinentHop) left the
        /// client in a dead InWorld state -- IsOpen() stayed false, so the rest
        /// of the game UI kept rendering as if still connected, and the login
        /// screen the player needs to sign into the NEW server never
        /// reappeared (see docs/TERRAFRONT_MULTIMAP.md section 4, gap #3).
        /// Called from TFClientNet::Disconnect(). Keeps the typed username
        /// (convenience — same server/account is the common case) but clears
        /// the password, character list/selection, and any in-flight error so
        /// the login screen renders fresh rather than showing stale state from
        /// the previous session.
        void ResetToLogin();

        // --- reply sinks: Task 6 wires TFClientNet's onboarding handlers to call
        // these directly once `m_ctx->loginFlow` exists. Until then, Update()'s
        // getter poll calls the same methods internally so the flow still works
        // standalone/loopback pre-Task-6. ---
        void OnLoginReply(bool ok, uint8_t err, uint64_t accountId);
        void OnRegisterReply(bool ok, uint8_t err);
        void OnCharList(const TF_CharListReply& reply);
        void OnCharOpReply(bool ok, uint8_t err, uint64_t charId);
        void OnEnteredWorld();

      private:
        // ImGui internals (stubbed out when !SPARK_HAS_IMGUI, TFSpawnScreen/TFHUD
        // pattern).
        void RenderLoginScreen(float panelX, float panelY, float panelW, float panelH);
        // W11 server-browser lane: the LAN SERVERS list inside the login screen.
        void RenderLanServerList(float x, float y, float w, float h, bool blocked);
        void RenderCharacterSelectScreen(float panelX, float panelY, float panelW, float panelH);
        void RenderCharacterCreateScreen(float panelX, float panelY, float panelW, float panelH);
        void RenderEnteringWorldScreen(float panelX, float panelY, float panelW, float panelH);

        // W11 server-browser lane: run the existing tf_connect path
        // (TFWorldSetup::Connect) against a discovered LAN server.
        void JoinLanServer(const std::string& ip, uint16_t port);

        // Sends
        void SendLogin();
        void SendRegister();
        void SendCharList();
        void SendCharCreate();
        void SendCharDelete(uint64_t charId);
        void SendEnterWorld(uint64_t charId);

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        bool m_showDebug{false};

        TFFlowState m_state{TFFlowState::Login};
        std::string m_error;

        // Login / Register form (sizes mirror TF_AuthRequest's wire fields).
        char m_username[32]{};
        char m_password[64]{};

        // Session
        uint64_t m_accountId{0};

        // Character select
        std::vector<TF_CharBrief> m_chars;
        int m_selectedIdx{-1};

        // Character create (name size mirrors TF_CharCreateRequest::name).
        char m_createName[24]{};
        FactionId m_createFaction{FactionId::MRA};

        // Entering-world splash
        float m_enterTimer{0.0f};

        // Pending-request tracking: disables the relevant buttons while a reply
        // is in flight. Cleared by the reply sinks themselves (Task 6 — TFClientNet
        // forwards replies directly instead of the old getter-poll fallback).
        enum class PendingOp : uint8_t
        {
            None,
            Login,
            Register,
            CharList,
            CharCreate,
            CharDelete
        };
        PendingOp m_pending{PendingOp::None};

        // W11 server-browser lane: LAN beacon (server) + scanner (client), owned
        // here because this Update runs on every role — see the header comment.
        TFLanDiscovery m_lan;
    };

} // namespace Terrafront
