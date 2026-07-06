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
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Terrafront {

enum class TFFlowState : uint8_t {
    Login,
    Register,
    CharSelect,
    CharCreate,
    EnteringWorld,
    InWorld,
};

class TFLoginFlow {
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
    bool        IsOpen() const { return m_state != TFFlowState::InWorld; }
    TFFlowState State() const { return m_state; }

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
    void RenderCharacterSelectScreen(float panelX, float panelY, float panelW, float panelH);
    void RenderCharacterCreateScreen(float panelX, float panelY, float panelW, float panelH);
    void RenderEnteringWorldScreen(float panelX, float panelY, float panelW, float panelH);

    // Sends
    void SendLogin();
    void SendRegister();
    void SendCharList();
    void SendCharCreate();
    void SendCharDelete(uint64_t charId);
    void SendEnterWorld(uint64_t charId);

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
    bool           m_showDebug{false};

    TFFlowState m_state{TFFlowState::Login};
    std::string m_error;

    // Login / Register form (sizes mirror TF_AuthRequest's wire fields).
    char m_username[32]{};
    char m_password[64]{};

    // Session
    uint64_t m_accountId{0};

    // Character select
    std::vector<TF_CharBrief> m_chars;
    int                       m_selectedIdx{-1};

    // Character create (name size mirrors TF_CharCreateRequest::name).
    char      m_createName[24]{};
    FactionId m_createFaction{FactionId::MRA};

    // Entering-world splash
    float m_enterTimer{0.0f};

    // Pending-request tracking: disables the relevant buttons while a reply
    // is in flight. Cleared by the reply sinks themselves (Task 6 — TFClientNet
    // forwards replies directly instead of the old getter-poll fallback).
    enum class PendingOp : uint8_t { None, Login, Register, CharList, CharCreate, CharDelete };
    PendingOp m_pending{PendingOp::None};
};

} // namespace Terrafront
