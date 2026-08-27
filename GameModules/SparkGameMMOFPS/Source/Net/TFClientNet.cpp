/**
 * @file TFClientNet.cpp
 * @brief W1 client networking core: connection observe, identity, and the
 *        client->server send path. The 60 Hz input pump with ClientPrediction
 *        (TFMoveStep simulator) and TF_MoveState reconciliation live in
 *        TFClientNetInput.cpp; TFMsg handlers in TFClientNetHandlers.cpp,
 *        loopback routing in TFClientNetLoopback.cpp, remote-pawn
 *        interpolation, first-person camera and the debug panel in
 *        TFClientNetView.cpp (same class, split per repo file-size rules).
 */
#include "Net/TFClientNet.h"
#include "Net/TFClientSessionEnd.h"
#include "Net/TFChatRules.h"

#include "Game/TFPlayerSystem.h"
#include "Net/TFServerSim.h"
#include "UI/TFLoginFlow.h"   // W5 onboarding (Task 6): loginFlow->IsOpen() input suppression
#include "UI/TFChatWindow.h"  // chat-social lane: chat window input suppression
#include "UI/TFSocialPanel.h" // chat-social lane: social panel input suppression
#include "UI/TFHUD.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFWorldSetup.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    TFClientNet::TFClientNet() = default;
    TFClientNet::~TFClientNet()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFClientNet::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        m_prediction.SetMaxPendingInputs(128);
        m_prediction.SetMovementSimulator([this](Spark::PredictedState& s, const Spark::PredictedInput& in, float dt)
                                          { SimulateMove(s, in, dt); });

        // Authority-with-local-player feedback (listen host / standalone): the
        // network HitConfirm/KillEvent path never reaches the in-process player,
        // so mirror it from the server-side event bus.
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnBusPlayerKilled(ev); });
        events.Subscribe<EvPlayerDamaged>([this](const EvPlayerDamaged& ev) { OnBusPlayerDamaged(ev); });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFClientNet initialized");
        return true;
    }

    void TFClientNet::Shutdown()
    {
        if (!m_initialized)
            return;
        Disconnect();
        m_interp.clear();
        m_initialized = false;
    }

    void TFClientNet::FixedUpdate(float) {}

    // ---------------------------------------------------------------------------
    // Frame update
    // ---------------------------------------------------------------------------

    void TFClientNet::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        m_clock += deltaTime;
        for (ChatLine& line : m_chatHistory)
            line.visibleFor = std::max(0.0f, line.visibleFor - deltaTime);
        UpdateConnectionState();

        if (!m_ctx->HasLocalPlayer())
            return; // dedicated server: no local input/camera/HUD feed

        // Local pawn state drives input, camera, and capture.
        PawnInfo pawn{};
        bool alive = false;
        if (m_ctx->players && m_ctx->localPlayer != kInvalidPlayer &&
            m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
            alive = pawn.alive;

        if (alive && !m_wasAlive)
        {
            RefreshClassSpeeds(pawn.cls);
            if (InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr)
                input->CaptureMouse(true);
        }
        else if (!alive && m_wasAlive)
        {
            if (InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr)
                input->CaptureMouse(false);
        }
        m_wasAlive = alive;

        // Fullscreen UIs (map / deploy screen / W5 login flow) own the mouse:
        // suspend fire + look input so a rally click (or a login button) doesn't
        // discharge the weapon underneath.
        const bool uiOpen = (m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
                            (m_ctx->loginFlow && m_ctx->loginFlow->IsOpen()) ||
                            (m_ctx->hud && m_ctx->hud->IsChatOpen()) || (m_chatUI && m_chatUI->IsOpen()) ||
                            (m_socialUI && m_socialUI->IsOpen());
        PumpInput(deltaTime, alive && !uiOpen);

        if (!m_ctx->IsAuthority())
        {
            ReconcileFromServer();
            UpdateRemotePawns();
        }

        DriveFirstPersonCamera(alive, pawn.pos);
    }

    // ---------------------------------------------------------------------------
    // Connection state / identity
    // ---------------------------------------------------------------------------

    bool TFClientNet::LocalLoopback() const
    {
        return m_ctx && m_ctx->IsAuthority() && m_ctx->HasLocalPlayer();
    }

    void TFClientNet::EnsureLocalHostIdentity()
    {
        if (m_ctx->localPlayer == kInvalidPlayer)
        {
            m_ctx->localPlayer = kTFLocalHostPlayer;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] local host player registered (id 0x%08X)",
                           kTFLocalHostPlayer);
        }
        m_localPlayer = m_ctx->localPlayer;

        // A faction picked before/without a connection (tf_faction while
        // standalone) must reach the authoritative registries.
        if (m_ctx->localFaction != FactionId::None && m_ctx->serverSim &&
            m_ctx->serverSim->GetPlayerFaction(m_localPlayer) == FactionId::None)
        {
            m_ctx->serverSim->SetPlayerFaction(m_localPlayer, m_ctx->localFaction);
            if (m_ctx->players)
                m_ctx->players->ServerHandleFactionSelect(m_localPlayer, m_ctx->localFaction);
        }
    }

    void TFClientNet::UpdateConnectionState()
    {
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        const bool clientUp = nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Client &&
                              nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;

        if (m_ctx->role == NetRole::Client && clientUp)
        {
            if (!m_connected)
            {
                m_connected = true;
                RegisterClientHandlers();
                m_predActive = false;
                m_interp.clear();
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client link up — awaiting TF_WorldWelcome");
            }
        }
        else if (m_connected)
        {
            m_connected = false;
            ReleaseClientHandlers();
            m_localPlayer = kInvalidPlayer;
            m_predActive = false;
            m_interp.clear();
            ResetSessionState();
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client link down");
        }
#endif

        if (!m_connected && LocalLoopback())
            EnsureLocalHostIdentity();
    }

    bool TFClientNet::IsConnected() const
    {
        // The loopback session counts as "connected" so the whole client-side
        // stack (tf_spawn, HUD deploy, weapon fire) works in a single process.
        return m_connected || LocalLoopback();
    }

    PlayerId TFClientNet::LocalPlayerId() const
    {
        return m_localPlayer;
    }

    bool TFClientNet::Connect(const std::string& ip, uint16_t port)
    {
#ifdef ENABLE_NETWORKING
        // TFWorldSetup owns NetworkManager boot/teardown; route through it so the
        // module keeps exactly one message pump. Update() observes the result.
        if (m_ctx && m_ctx->world)
            return m_ctx->world->Connect(ip, port);

        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() && !nm.Initialize())
            return false;
        if (!nm.Connect(ip, port, "TerrafrontPlayer"))
            return false;
        if (m_ctx)
            m_ctx->role = NetRole::Client;
        return true;
#else
        (void)ip;
        (void)port;
        SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] Connect: built without ENABLE_NETWORKING");
        return false;
#endif
    }

    void TFClientNet::Disconnect()
    {
#ifdef ENABLE_NETWORKING
        // A loopback listen host has no socket leave event, so explicitly run
        // the same authoritative cleanup used for remote disconnects before
        // invalidating the local identity or stopping NetworkManager.
        if (LocalLoopback() && m_localPlayer != kInvalidPlayer && m_ctx->serverSim)
            m_ctx->serverSim->DebugSimulateDisconnect(m_localPlayer);

        if (m_handlersRegistered)
            ReleaseClientHandlers();
#endif
        m_connected = false;
        m_localPlayer = kInvalidPlayer;
        m_predActive = false;
        m_interp.clear();
        m_chatHistory.clear();
        ResetSessionState();
    }

    void TFClientNet::ResetSessionState()
    {
        m_session.Reset();
        if (m_ctx)
        {
            const bool loginFlowAtLogin = !m_ctx->loginFlow || m_ctx->loginFlow->State() == TFFlowState::Login;
            const TFClientSessionEndDecision decision = PlanClientSessionEnd(m_ctx->role, loginFlowAtLogin);
            m_ctx->inWorld = false;
            m_ctx->localPlayer = kInvalidPlayer;
            m_ctx->localFaction = FactionId::None;
            m_ctx->role = decision.role;
            // Both an explicit disconnect and an observed link loss end the
            // onboarding session. Keep the typed username only; clear the
            // password, character selection, and stale InWorld UI state.
            if (m_ctx->loginFlow && decision.resetLoginFlow)
                m_ctx->loginFlow->ResetToLogin();
        }
    }

    // ---------------------------------------------------------------------------
    // Sends (network path; loopback routing lives in TFClientNetHandlers.cpp)
    // ---------------------------------------------------------------------------

    void TFClientNet::SendInput(const TF_ClientInput& input)
    {
        SendMsg(TFMsg::ClientInput, &input, sizeof(input));
    }

    bool TFClientNet::SendChat(ChatChannel channel, const std::string& text)
    {
        if (!IsValidChatChannel(static_cast<uint8_t>(channel)) || !IsConnected() || !m_ctx || !m_ctx->InWorld())
            return false;

        TF_ChatMsg msg{};
        msg.channel = static_cast<uint8_t>(channel);
        if (!NormalizeChatText(text.data(), text.size(), msg.text, sizeof(msg.text)))
            return false;
        SendMsg(TFMsg::ChatMsg, &msg, sizeof(msg));
        return true;
    }

    void TFClientNet::SendMsg(TFMsg id, const void* payload, size_t size)
    {
        if (!m_initialized || !m_ctx)
            return;

        // Local record of the class we asked for (SpawnReply does not echo it).
        if (id == TFMsg::SpawnRequest && size == sizeof(TF_SpawnRequest) && m_ctx->players)
        {
            TF_SpawnRequest rq;
            std::memcpy(&rq, payload, sizeof(rq));
            m_ctx->players->ClientNoteRequestedClass(static_cast<ClassId>(rq.classId));
        }

        if (!m_connected)
        {
            if (LocalLoopback())
                RouteLoopback(id, payload, size);
            return;
        }

#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(static_cast<uint16_t>(id));
        msg.channel =
            (id == TFMsg::ClientInput) ? Spark::Net::ChannelType::Unreliable : Spark::Net::ChannelType::Reliable;
        msg.sensitive = (id == TFMsg::LoginRequest || id == TFMsg::RegisterRequest);
        msg.localOnly = msg.sensitive;
        msg.payload.resize(size);
        if (size > 0)
            std::memcpy(msg.payload.data(), payload, size);
        nm.SendMessage(msg);
#endif
    }

    // Input pump + prediction (PumpInput/SendOneInput/SimulateMove/
    // RefreshClassSpeeds/SeedPredictionAt/ReconcileFromServer/
    // GetPredictedLocalState) live in TFClientNetInput.cpp.

} // namespace Terrafront
