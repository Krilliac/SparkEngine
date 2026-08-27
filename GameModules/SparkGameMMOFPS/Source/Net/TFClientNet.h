/**
 * @file TFClientNet.h
 * @brief Client connection, ClientPrediction wiring, interpolation buffers.
 *
 * OWNERSHIP: this header + TFClientNet.cpp belong to ONE implementation agent.
 * The lifecycle + the FROZEN cross-system API below are the module contract.
 *
 * W1 full implementation:
 *  - Connection observe: TFWorldSetup::Connect boots NetworkManager in client
 *    role; this system detects the live connection in Update, registers the
 *    client-side TFMsg handlers (WorldWelcome/SpawnReply/HitConfirm/
 *    DamageEvent/KillEvent/XPEvent) and runs the TF handshake.
 *  - Input pump: samples InputManager at up to 60 Hz fixed steps, builds
 *    TF_ClientInput (seq/buttons/move axes/view angles) and feeds the SAME
 *    input to Spark::ClientPrediction with a TFMoveStep simulator
 *    (Game/TFMovementModel.h — the shared client/server movement contract).
 *  - Reconcile: consumes TFReplication's owner-only TF_MoveState feed
 *    (HasFreshMoveState/GetLatestMoveState) and calls ClientPrediction::
 *    Reconcile with the acked input sequence.
 *  - Remote pawns: 100 ms interpolation-delay buffers over the RemotePawn
 *    store; writes the smoothed pose into the local visual entities that
 *    TFPlayerSystem::SyncClientRecords creates.
 *  - First-person camera: mouse look drives the engine camera (FPS-module
 *    convention); the camera is positioned at the predicted pawn eye
 *    (TFWeaponMath::kEyeHeightM above the feet).
 *  - Listen-host / standalone loopback: when the local process is the
 *    authority, SendMsg/SendInput skip the socket entirely and route straight
 *    into TFServerSim / TFPlayerSystem / TFWeaponSystem, so a single process
 *    plays the full loop with no network.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFClientSessionState.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFRepProtocol.h"

#include "Engine/Networking/ClientPrediction.h"
#include "Engine/Networking/InterpolationBuffer.h"
#include "Engine/Networking/NetworkClientId.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace Terrafront
{

    class TFChatWindow;  // UI/TFChatWindow.h (chat-social lane)
    class TFSocialPanel; // UI/TFSocialPanel.h (chat-social lane)

    /// PlayerId used for the in-process local player on authority roles
    /// (listen host / standalone). Deliberately far above NetworkManager's
    /// incrementing client ids and distinct from kInvalidPlayer.
    constexpr PlayerId kTFLocalHostPlayer = Spark::Net::FIRST_RESERVED_CLIENT_ID + 1u;
    static_assert(kTFLocalHostPlayer == 0xFFFFFF01u);
    static_assert(Spark::Net::IsReservedClientID(kTFLocalHostPlayer));

    class TFClientNet
    {
      public:
        struct ChatLine
        {
            PlayerId from{kInvalidPlayer};
            ChatChannel channel{ChatChannel::Region};
            std::string text;
            float visibleFor{0.0f};
        };

        TFClientNet();
        ~TFClientNet();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- FROZEN cross-system API (W1) ---
        bool IsConnected() const;
        PlayerId LocalPlayerId() const;
        void SendInput(const TF_ClientInput& input);
        void SendMsg(TFMsg id, const void* payload, size_t size);
        bool SendChat(ChatChannel channel, const std::string& text);
        const std::deque<ChatLine>& ChatHistory() const { return m_chatHistory; }

        /// Connect to a remote host (called via TFWorldSetup::Connect).
        bool Connect(const std::string& ip, uint16_t port);
        void Disconnect();

        // --- W1 additions beyond the frozen surface (documented in wave report) ---

        /// Pure client: the locally-predicted state of the OWN pawn (feet position,
        /// velocity, view angles). Returns false on authority roles / when no
        /// predicted pawn exists; TFPlayerSystem uses it to serve PawnInfo for the
        /// local player without waiting a replication round-trip.
        bool GetPredictedLocalState(float outPos[3], float outVel[3], float& outYaw, float& outPitch) const;

        /// Debug panel toggle (hidden by default; wired from tf_* console commands).
        void ToggleDebugUI() { m_showDebug = !m_showDebug; }

        // --- chat-social lane (additive): UI self-registration ------------------
        // TFChatWindow/TFSocialPanel call these from their Initialize so the
        // uiOpen input-suppression check in Update covers them (a chat line or
        // friend click must not fire the weapon underneath). Deliberately NOT
        // routed through TFGameContext so this lane's files compile before the
        // integrator wires Main.cpp.
        void SetChatUI(TFChatWindow* chat) { m_chatUI = chat; }
        void SetSocialPanel(TFSocialPanel* panel) { m_socialUI = panel; }

        // --- W5 onboarding (Task 4) reply stash --------------------------------
        // TFLoginFlow (Task 5) does not exist yet; these getters expose the last
        // server reply so console commands (tf_login/tf_char_list/...) and Task
        // 5/6 can consume it. Once `m_ctx->loginFlow` is wired (Task 6), the
        // On*Reply handlers below should forward to it directly instead.
        bool IsLoggedIn() const { return m_session.loggedIn; }
        uint64_t AccountId() const { return m_session.accountId; }
        uint8_t LastAuthError() const { return static_cast<uint8_t>(m_session.lastAuthError); }
        const std::vector<TF_CharBrief>& CharacterList() const { return m_session.characters; }
        uint8_t LastCharOpError() const { return static_cast<uint8_t>(m_session.lastCharacterError); }
        uint64_t LastCharOpId() const { return m_session.lastCharacterId; }

#ifdef ENABLE_NETWORKING
        /// W5 onboarding (Task 7 acceptance-harness fix): the listen-host/
        /// standalone local player (kTFLocalHostPlayer) never has a real
        /// NetworkManager socket address (TFClientNet::RouteLoopback bypasses the
        /// socket entirely for the C->S direction) so TFServerSim::SendToPlayer's
        /// normal nm.SendToClient() path finds no address for it and silently
        /// drops the reply. Every onboarding client-state transition (logged-in,
        /// character list, in-world) is driven purely by these S->C replies --
        /// unlike movement/spawn there is no ECS ground truth the local player
        /// could read directly instead -- so that delivery gap silently broke the
        /// whole login->world flow for local/standalone play. TFServerSim::
        /// SendToPlayer calls this in-process for that one player instead of
        /// going through the (nonexistent) socket, dispatching to the exact same
        /// On*Reply handlers RegisterClientHandlers wires to NetworkManager.
        void DeliverLoopbackReply(TFMsg id, const void* data, size_t size);
#endif

      private:
        // Per-remote-entity interpolation buffers (100 ms render delay).
        struct InterpEntry
        {
            Spark::Net::InterpolationBuffer<DirectX::XMFLOAT3> pos;
            Spark::Net::InterpolationBuffer<float> yaw;
            float lastYaw{0.0f};       ///< unwrapped yaw of the newest sample
            double lastRecvTime{-1.0}; ///< RemotePawn.recvTime of the newest sample
            bool has{false};
        };

        bool LocalLoopback() const; ///< authority role with a local player
        void EnsureLocalHostIdentity();
        void UpdateConnectionState();
        void ResetSessionState();
        void RouteLoopback(TFMsg id, const void* payload, size_t size);

        void PumpInput(float dt, bool aliveLocalPawn);
        void SendOneInput(float moveX, float moveY, uint16_t buttons);
        void ReconcileFromServer();
        void UpdateRemotePawns();
        void DriveFirstPersonCamera(bool aliveLocalPawn, const float feetPos[3]);
        void SeedPredictionAt(const float pos[3]);
        void SimulateMove(Spark::PredictedState& s, const Spark::PredictedInput& in, float dt) const;
        void RefreshClassSpeeds(ClassId cls);

        void OnBusPlayerKilled(const EvPlayerKilled& ev);
        void OnBusPlayerDamaged(const EvPlayerDamaged& ev);
        void PushKillfeedEntry(PlayerId killer, PlayerId victim, WeaponId weapon, FactionId killerF, FactionId victimF,
                               bool headshot);

#ifdef ENABLE_NETWORKING
        void RegisterClientHandlers();
        void ReleaseClientHandlers();
        void OnWorldWelcome(const void* data, size_t size);
        void OnSpawnReply(const void* data, size_t size);
        void OnHitConfirm(const void* data, size_t size);
        void OnDamageEvent(const void* data, size_t size);
        void OnKillEvent(const void* data, size_t size);
        void OnXPEvent(const void* data, size_t size);
        void OnChatMsg(const void* data, size_t size);

        // W5 onboarding (Task 4). TFLoginFlow (Task 5) is not wired yet — these
        // parse + stash the reply so Task 5/6 can read it via a getter, or replace
        // this stash entirely once `m_ctx->loginFlow` exists (Task 6). Logged at
        // INFO so the loopback flow is observable before the UI lands.
        void OnLoginReply(const void* data, size_t size);
        void OnRegisterReply(const void* data, size_t size);
        void OnCharListReply(const void* data, size_t size);
        void OnCharCreateReply(const void* data, size_t size);
        void OnCharDeleteReply(const void* data, size_t size);
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        TFChatWindow* m_chatUI{nullptr};    // chat-social lane (self-registered)
        TFSocialPanel* m_socialUI{nullptr}; // chat-social lane (self-registered)
        bool m_initialized{false};
        bool m_connected{false};
        PlayerId m_localPlayer{kInvalidPlayer};
        uint32_t m_inputSeq{0}; ///< loopback-only sequence counter

        double m_clock{0.0};      ///< monotonic client clock (Update)
        float m_inputAccum{0.0f}; ///< 60 Hz input pacing accumulator
        bool m_handlersRegistered{false};
        bool m_wasAlive{false};
        std::deque<ChatLine> m_chatHistory;

        // View angles (camera convention, radians; see TFMovementModel.h basis).
        float m_viewYaw{0.0f};
        float m_viewPitch{0.0f};

        // Prediction (pure client only)
        Spark::ClientPrediction m_prediction;
        Spark::PredictedState m_predState{};
        bool m_predActive{false};
        float m_runSpeed{5.2f};
        float m_sprintSpeed{7.2f};

        // Remote pawn interpolation
        std::unordered_map<EntityId, InterpEntry> m_interp;

        // Stats / debug
        uint32_t m_inputsSent{0};
        uint32_t m_reconciles{0};
        uint16_t m_lastRank{1};
        uint32_t m_lastXPTotal{0};
        bool m_showDebug{false};

        // W5 onboarding (Task 4) reply stash (see the getters above).
        TFClientSessionState m_session;
    };

} // namespace Terrafront
