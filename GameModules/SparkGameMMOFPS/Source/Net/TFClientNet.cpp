/**
 * @file TFClientNet.cpp
 * @brief W1 client networking core: connection observe, 60 Hz input pump with
 *        ClientPrediction (TFMoveStep simulator) and TF_MoveState
 *        reconciliation. TFMsg handlers, loopback routing, remote-pawn
 *        interpolation, first-person camera and the debug panel live in
 *        TFClientNetHandlers.cpp (same class, split per repo file-size rules).
 */
#include "Net/TFClientNet.h"

#include "Data/TFDataTables.h"
#include "Game/TFMovementModel.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponSystem.h"
#include "Net/TFReplication.h"
#include "Net/TFServerSim.h"
#include "UI/TFLoginFlow.h"   // W5 onboarding (Task 6): loginFlow->IsOpen() input suppression
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFWorldSetup.h"

#include "Camera/SparkEngineCamera.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront {

namespace {

constexpr float kInputStepSec     = 1.0f / 60.0f; // input send / predict cadence
constexpr int   kMaxStepsPerFrame = 4;            // catch-up bound after a hitch
constexpr float kMouseSens        = 0.005f;       // radians per count (FPS module value)

// Windows virtual-key codes (numeric so no <windows.h> dependency here).
constexpr int kVkShift   = 0x10;
constexpr int kVkControl = 0x11;
constexpr int kVkSpace   = 0x20;

} // namespace

TFClientNet::TFClientNet() = default;
TFClientNet::~TFClientNet() { if (m_initialized) Shutdown(); }

bool TFClientNet::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;

    m_prediction.SetMaxPendingInputs(128);
    m_prediction.SetMovementSimulator(
        [this](Spark::PredictedState& s, const Spark::PredictedInput& in, float dt)
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
    const bool uiOpen = (m_ctx->map && m_ctx->map->IsOpen()) ||
                        (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
                        (m_ctx->loginFlow && m_ctx->loginFlow->IsOpen());
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
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] local host player registered (id 0x%08X)", kTFLocalHostPlayer);
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
    const bool clientUp = nm.IsInitialized() &&
                          nm.GetRole() == Spark::Net::NetworkRole::Client &&
                          nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;

    if (m_ctx->role == NetRole::Client && clientUp)
    {
        if (!m_connected)
        {
            m_connected = true;
            RegisterClientHandlers();
            m_predActive = false;
            m_interp.clear();
            SPARK_LOG_INFO(Spark::LogCategory::Game,
                           "[TF] client link up — awaiting TF_WorldWelcome");
        }
    }
    else if (m_connected)
    {
        m_connected = false;
        ReleaseClientHandlers();
        m_localPlayer = kInvalidPlayer;
        m_predActive = false;
        m_interp.clear();
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
    (void)ip; (void)port;
    SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] Connect: built without ENABLE_NETWORKING");
    return false;
#endif
}

void TFClientNet::Disconnect()
{
#ifdef ENABLE_NETWORKING
    if (m_handlersRegistered)
        ReleaseClientHandlers();
#endif
    m_connected = false;
    m_localPlayer = kInvalidPlayer;
    m_predActive = false;
    m_interp.clear();
    if (m_ctx && m_ctx->role == NetRole::Client)
        m_ctx->role = NetRole::Standalone;
}

// ---------------------------------------------------------------------------
// Sends (network path; loopback routing lives in TFClientNetHandlers.cpp)
// ---------------------------------------------------------------------------

void TFClientNet::SendInput(const TF_ClientInput& input)
{
    SendMsg(TFMsg::ClientInput, &input, sizeof(input));
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
    msg.channel = (id == TFMsg::ClientInput) ? Spark::Net::ChannelType::Unreliable
                                             : Spark::Net::ChannelType::Reliable;
    msg.payload.resize(size);
    if (size > 0)
        std::memcpy(msg.payload.data(), payload, size);
    nm.SendMessage(msg);
#endif
}

// ---------------------------------------------------------------------------
// Input pump + prediction
// ---------------------------------------------------------------------------

void TFClientNet::PumpInput(float dt, bool aliveLocalPawn)
{
    if (!aliveLocalPawn || !IsConnected())
    {
        m_inputAccum = 0.0f;
        return;
    }

    InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
    if (!input)
        return;

    // --- mouse look (camera owns the angles; we read them back) -----------
    // Module-owned camera: the engine context camera slot is empty in module
    // mode (see TFWorldSetup::GetCamera).
    SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
    if (cam)
    {
        const auto [dx, dy] = input->GetMouseDelta();
        if (dx != 0)
            cam->Yaw(static_cast<float>(dx) * kMouseSens);
        if (dy != 0)
            cam->Pitch(static_cast<float>(-dy) * kMouseSens);
        const auto rot = cam->GetRotation(); // (pitch, yaw, roll) radians
        m_viewPitch = rot.x;
        m_viewYaw = rot.y;
    }

    // --- buttons + axes ----------------------------------------------------
    uint16_t buttons = 0;
    if (input->IsMouseButtonDown(0))
    {
        buttons |= TFB_Fire;
        if (m_ctx->weapons)
            m_ctx->weapons->ClientTriggerFire(); // RoF-paced internally
    }
    if (input->IsMouseButtonDown(1))  buttons |= TFB_AltFire;
    if (input->IsKeyDown(kVkSpace))   buttons |= TFB_Jump;
    if (input->IsKeyDown(kVkControl)) buttons |= TFB_Crouch;
    if (input->IsKeyDown(kVkShift))   buttons |= TFB_Sprint;
    if (input->WasKeyPressed('R'))    buttons |= TFB_Reload;
    if (input->IsKeyDown('E'))        buttons |= TFB_Interact;
    if (input->IsKeyDown('F'))        buttons |= TFB_Ability;

    float moveX = 0.0f, moveY = 0.0f;
    if (input->IsKeyDown('D')) moveX += 1.0f;
    if (input->IsKeyDown('A')) moveX -= 1.0f;
    if (input->IsKeyDown('W')) moveY += 1.0f;
    if (input->IsKeyDown('S')) moveY -= 1.0f;

    // --- fixed 60 Hz send/predict steps -------------------------------------
    m_inputAccum += dt;
    int steps = 0;
    while (m_inputAccum >= kInputStepSec && steps < kMaxStepsPerFrame)
    {
        m_inputAccum -= kInputStepSec;
        ++steps;
        SendOneInput(moveX, moveY, buttons);
        buttons = static_cast<uint16_t>(buttons & ~TFB_Reload); // one-shot per frame
    }
    if (steps == kMaxStepsPerFrame)
        m_inputAccum = 0.0f; // hitch: drop the backlog instead of bursting
}

void TFClientNet::SendOneInput(float moveX, float moveY, uint16_t buttons)
{
    uint32_t seq = 0;

    if (!m_ctx->IsAuthority())
    {
        // Record + locally apply through ClientPrediction so the sequence the
        // server acks in TF_MoveState matches our pending-input buffer.
        Spark::PredictedInput pi{};
        pi.timestamp = static_cast<float>(m_clock);
        pi.moveDirection = {moveX, 0.0f, moveY};
        pi.lookYaw = m_viewYaw;
        pi.lookPitch = m_viewPitch;
        pi.jump   = (buttons & TFB_Jump) != 0;
        pi.crouch = (buttons & TFB_Crouch) != 0;
        pi.sprint = (buttons & TFB_Sprint) != 0;
        pi.fire   = (buttons & TFB_Fire) != 0;
        pi.reload = (buttons & TFB_Reload) != 0;
        pi.interact = (buttons & TFB_Interact) != 0;

        seq = m_prediction.RecordInput(pi);
        pi.sequenceNumber = seq;
        m_prediction.ApplyPrediction(m_predState, pi, kInputStepSec);
        m_predActive = true;
    }
    else
    {
        seq = ++m_inputSeq;
    }

    TF_ClientInput in{};
    in.seq = seq;
    in.buttons = buttons;
    in.moveX = static_cast<int8_t>(std::clamp(moveX, -1.0f, 1.0f) * 127.0f);
    in.moveY = static_cast<int8_t>(std::clamp(moveY, -1.0f, 1.0f) * 127.0f);
    in.viewYaw = m_viewYaw;
    in.viewPitch = m_viewPitch;
    in.weaponSlot = 0; // TF-W2: mirror TFWeaponSystem's active slot

    SendInput(in);
    ++m_inputsSent;
}

void TFClientNet::SimulateMove(Spark::PredictedState& s, const Spark::PredictedInput& in,
                               float dt) const
{
    TFMoveState ms;
    ms.pos[0] = s.position.x; ms.pos[1] = s.position.y; ms.pos[2] = s.position.z;
    ms.vel[0] = s.velocity.x; ms.vel[1] = s.velocity.y; ms.vel[2] = s.velocity.z;
    ms.grounded = s.isGrounded;

    TFMoveInput mi;
    mi.moveX = in.moveDirection.x;
    mi.moveY = in.moveDirection.z;
    mi.yaw = in.lookYaw;
    mi.jump = in.jump;
    mi.sprint = in.sprint;
    mi.crouch = in.crouch;

    const TFWorldSetup* world = m_ctx ? m_ctx->world : nullptr;
    TFMoveStep(ms, mi, m_runSpeed, m_sprintSpeed, dt,
               [world](float x, float z) { return world ? world->TerrainHeightAt(x, z) : 0.0f; });

    s.position = {ms.pos[0], ms.pos[1], ms.pos[2]};
    s.velocity = {ms.vel[0], ms.vel[1], ms.vel[2]};
    s.isGrounded = ms.grounded;
    s.yaw = in.lookYaw;
    s.pitch = in.lookPitch;
    s.isCrouching = in.crouch;
    s.isSprinting = in.sprint;
}

void TFClientNet::RefreshClassSpeeds(ClassId cls)
{
    m_runSpeed = 5.2f;
    m_sprintSpeed = 7.2f;
    if (m_ctx && m_ctx->data && m_ctx->data->IsLoaded())
    {
        if (const ClassDef* cd = m_ctx->data->GetClass(cls))
        {
            m_runSpeed = cd->runSpeed;
            m_sprintSpeed = cd->sprintSpeed;
        }
    }
}

void TFClientNet::SeedPredictionAt(const float pos[3])
{
    m_prediction = Spark::ClientPrediction{};
    m_prediction.SetMaxPendingInputs(128);
    m_prediction.SetMovementSimulator(
        [this](Spark::PredictedState& s, const Spark::PredictedInput& in, float dt)
        { SimulateMove(s, in, dt); });

    m_predState = Spark::PredictedState{};
    m_predState.position = {pos[0], pos[1], pos[2]};
    m_predState.isGrounded = true;
    m_predActive = true;
}

void TFClientNet::ReconcileFromServer()
{
    if (!m_ctx->replication || !m_predActive || !m_ctx->replication->HasFreshMoveState())
        return;

    TF_MoveState ms{};
    if (!m_ctx->replication->GetLatestMoveState(ms))
        return;

    Spark::PredictedState sv{};
    sv.lastProcessedInput = ms.lastAckedSeq;
    sv.position = {ms.posX, ms.posY, ms.posZ};
    sv.velocity = {ms.velX, ms.velY, ms.velZ};
    sv.yaw = ms.yaw;
    sv.pitch = ms.pitch;
    sv.isGrounded = ms.grounded != 0;

    m_prediction.Reconcile(sv, kInputStepSec);
    m_predState = m_prediction.GetState();
    ++m_reconciles;
}

bool TFClientNet::GetPredictedLocalState(float outPos[3], float outVel[3],
                                         float& outYaw, float& outPitch) const
{
    if (!m_predActive || !m_ctx || m_ctx->IsAuthority())
        return false;
    outPos[0] = m_predState.position.x;
    outPos[1] = m_predState.position.y;
    outPos[2] = m_predState.position.z;
    outVel[0] = m_predState.velocity.x;
    outVel[1] = m_predState.velocity.y;
    outVel[2] = m_predState.velocity.z;
    outYaw = m_viewYaw;
    outPitch = m_viewPitch;
    return true;
}

} // namespace Terrafront
