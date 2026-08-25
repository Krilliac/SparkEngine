/**
 * @file TFLoginFlow.cpp
 * @brief W5 onboarding client UI state machine (see TFLoginFlow.h): lifecycle,
 *        state transitions, reply sinks, and sends.
 *
 * The ImGui rendering half (RenderUI + per-state screens and their headless
 * stubs) lives in TFLoginFlowDraw.cpp, split per the repo file-size rule
 * (TFHUDDraw.cpp pattern — same class, feature-owned translation units).
 */
#include "UI/TFLoginFlow.h"

#include "Account/TFAccountSystem.h"   // TFAuthErr (error text only; no TFDatabase coupling used)
#include "Account/TFCharacterSystem.h" // TFCharErr
#include "Net/TFClientNet.h"
#include "World/TFWorldSetup.h" // W11 server-browser: Connect() = the tf_connect path

#include "Utils/LogMacros.h"
#include "Utils/ScopeGuard.h"
#include "Utils/SecureMemory.h"

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        const char* AuthErrText(uint8_t errByte)
        {
            switch (static_cast<TFAuthErr>(errByte))
            {
            case TFAuthErr::Ok:
                return "";
            case TFAuthErr::BadCredentials:
                return "Incorrect callsign or passphrase.";
            case TFAuthErr::UsernameTaken:
                return "That callsign is already taken.";
            case TFAuthErr::UsernameTooShort:
                return "Callsign must be at least 3 characters.";
            case TFAuthErr::PasswordTooShort:
                return "Passphrase is too short.";
            case TFAuthErr::ServerError:
                return "Server error - try again.";
            case TFAuthErr::NotLoggedIn:
                return "You are not logged in.";
            case TFAuthErr::SessionActive:
                return "Disconnect before changing accounts.";
            default:
                return "Unknown error.";
            }
        }

        const char* CharErrText(uint8_t errByte)
        {
            switch (static_cast<TFCharErr>(errByte))
            {
            case TFCharErr::Ok:
                return "";
            case TFCharErr::SlotsFull:
                return "All character slots are full.";
            case TFCharErr::NameTaken:
                return "That name is already taken.";
            case TFCharErr::NameInvalid:
                return "Invalid name (3-23 characters).";
            case TFCharErr::NoSuchCharacter:
                return "No such character.";
            case TFCharErr::NotYourCharacter:
                return "That character does not belong to you.";
            case TFCharErr::ServerError:
                return "Server error - try again.";
            case TFCharErr::NotLoggedIn:
                return "You are not logged in.";
            case TFCharErr::SessionActive:
                return "Leave the world before changing characters.";
            default:
                return "Unknown error.";
            }
        }

    } // namespace

    TFLoginFlow::TFLoginFlow() = default;
    TFLoginFlow::~TFLoginFlow()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFLoginFlow::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        m_state = TFFlowState::Login;
        std::memset(m_username, 0, sizeof(m_username));
        Spark::SecureErase(m_password, sizeof(m_password));
        std::memset(m_createName, 0, sizeof(m_createName));
        m_error.clear();
        m_chars.clear();
        m_selectedIdx = -1;
        m_pending = PendingOp::None;

        // W11 server-browser lane: LAN discovery is owned here because this
        // Update runs on every role (see TFLoginFlow.h). Never boot-fatal.
        if (!m_lan.Initialize(ctx, events))
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] TFLanDiscovery init failed - LAN browser off");

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFLoginFlow initialized");
        return true;
    }

    void TFLoginFlow::Shutdown()
    {
        m_lan.Shutdown(); // W11 server-browser lane
        Spark::SecureErase(m_password, sizeof(m_password));
        m_initialized = false;
    }

    void TFLoginFlow::ResetToLogin()
    {
        // See the header comment: called on client disconnect (manual
        // tf_disconnect or a continent hop) so the login screen reappears
        // instead of leaving the game UI believing it is still InWorld.
        m_state = TFFlowState::Login;
        Spark::SecureErase(m_password, sizeof(m_password));
        m_error.clear();
        m_chars.clear();
        m_selectedIdx = -1;
        m_pending = PendingOp::None;
        m_accountId = 0;
        m_enterTimer = 0.0f;
        // m_username intentionally preserved (same server/account is the
        // common case — re-typing it every hop is pure friction). m_lan is
        // untouched: it self-arms off ctx.role and the login screen render
        // condition, not this flow's m_state.
    }

    void TFLoginFlow::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        if (m_state == TFFlowState::EnteringWorld)
            m_enterTimer += deltaTime;

        // W11 server-browser lane: ticks the server beacon (self-arming off
        // ctx.role) and drains/expires scanner results. The scanner itself is only
        // ARMED from RenderLoginScreen, so it stops once the login screen is left
        // (and never starts at all in headless runs, where RenderUI is a stub).
        //
        // W13 multimap follow-up (docs/TERRAFRONT_MULTIMAP.md section 5.1):
        // once InWorld, re-arm it ourselves (RenderLoginScreen no longer
        // renders — TFLoginFlow::RenderUI early-outs on InWorld) so
        // TFTravelSystem::RenderUI's continent-hop terminal has a live feed
        // to prefer over the static continents.json host/port. Gated to
        // NetRole::Client: that's the only role that can ever continent-hop
        // (see TFTravelSystem::ClientRequestContinentHop), so a listen host /
        // dedicated server / standalone loopback never binds the extra
        // socket for no reason. Every other state (CharSelect/CharCreate/
        // EnteringWorld, or InWorld on a non-Client role) stops it exactly as
        // before.
        m_lan.Update(deltaTime);
        if (m_state == TFFlowState::Login || m_state == TFFlowState::Register)
        {
            // armed by RenderLanServerList while this state actually renders
        }
        else if (m_state == TFFlowState::InWorld && m_ctx->role == NetRole::Client)
        {
            m_lan.StartScanning();
        }
        else
        {
            m_lan.StopScanning();
        }
    }

    // ---------------------------------------------------------------------------
    // Reply sinks — called directly by TFClientNet's onboarding handlers
    // (TFClientNetHandlers.cpp) via m_ctx->loginFlow (Task 6).
    // ---------------------------------------------------------------------------

    void TFLoginFlow::OnLoginReply(bool ok, uint8_t err, uint64_t accountId)
    {
        m_pending = PendingOp::None;
        if (ok)
        {
            m_accountId = accountId;
            m_error.clear();
            m_selectedIdx = -1;
            SendCharList();
            m_state = TFFlowState::CharSelect;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] login ok, account %llu",
                           static_cast<unsigned long long>(accountId));
        }
        else
        {
            m_error = AuthErrText(err);
        }
    }

    void TFLoginFlow::OnRegisterReply(bool ok, uint8_t err)
    {
        m_pending = PendingOp::None;
        if (ok)
        {
            m_error = "Account created - sign in below.";
            m_state = TFFlowState::Login;
        }
        else
        {
            m_error = AuthErrText(err);
        }
    }

    void TFLoginFlow::OnCharList(const TF_CharListReply& reply)
    {
        m_pending = PendingOp::None;
        m_chars.assign(reply.chars, reply.chars + std::min<uint8_t>(reply.count, 5));
        if (m_selectedIdx >= static_cast<int>(m_chars.size()))
            m_selectedIdx = -1;
    }

    void TFLoginFlow::OnCharOpReply(bool ok, uint8_t err, uint64_t charId)
    {
        (void)charId;
        m_pending = PendingOp::None;
        if (ok)
        {
            m_error.clear();
            SendCharList();
            m_state = TFFlowState::CharSelect;
        }
        else
        {
            m_error = CharErrText(err);
        }
    }

    void TFLoginFlow::OnEnteredWorld()
    {
        // m_ctx->inWorld is set by the caller (TFClientNet::OnWorldWelcome, the
        // gated enter-world reply) right before/after this call — see
        // TFClientNetHandlers.cpp and TFTypes.h TFGameContext::inWorld.
        m_state = TFFlowState::InWorld;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] entered world");
    }

    // ---------------------------------------------------------------------------
    // Sends
    // ---------------------------------------------------------------------------

    void TFLoginFlow::SendLogin()
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_AuthRequest req{};
        const auto clearRequest = Spark::MakeScopeExit([&] { Spark::SecureErase(&req, sizeof(req)); });
        std::strncpy(req.user, m_username, sizeof(req.user) - 1);
        std::strncpy(req.pass, m_password, sizeof(req.pass) - 1);
        m_error.clear();
        DispatchAfterArmingOnboardingState([&] { m_pending = PendingOp::Login; },
                                           [&] { m_ctx->clientNet->SendMsg(TFMsg::LoginRequest, &req, sizeof(req)); });
        Spark::SecureErase(m_password, sizeof(m_password));
    }

    void TFLoginFlow::SendRegister()
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_AuthRequest req{};
        const auto clearRequest = Spark::MakeScopeExit([&] { Spark::SecureErase(&req, sizeof(req)); });
        std::strncpy(req.user, m_username, sizeof(req.user) - 1);
        std::strncpy(req.pass, m_password, sizeof(req.pass) - 1);
        m_error.clear();
        DispatchAfterArmingOnboardingState([&] { m_pending = PendingOp::Register; }, [&]
                                           { m_ctx->clientNet->SendMsg(TFMsg::RegisterRequest, &req, sizeof(req)); });
        Spark::SecureErase(m_password, sizeof(m_password));
    }

    void TFLoginFlow::SendCharList()
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        DispatchAfterArmingOnboardingState([&] { m_pending = PendingOp::CharList; },
                                           [&] { m_ctx->clientNet->SendMsg(TFMsg::CharListRequest, nullptr, 0); });
    }

    void TFLoginFlow::SendCharCreate()
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_CharCreateRequest req{};
        std::strncpy(req.name, m_createName, sizeof(req.name) - 1);
        req.faction = static_cast<uint8_t>(m_createFaction);
        m_error.clear();
        DispatchAfterArmingOnboardingState([&] { m_pending = PendingOp::CharCreate; },
                                           [&] { m_ctx->clientNet->SendMsg(TFMsg::CharCreateReq, &req, sizeof(req)); });
    }

    void TFLoginFlow::SendCharDelete(uint64_t charId)
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_CharDeleteRequest req{};
        req.charId = charId;
        m_error.clear();
        DispatchAfterArmingOnboardingState([&] { m_pending = PendingOp::CharDelete; },
                                           [&] { m_ctx->clientNet->SendMsg(TFMsg::CharDeleteReq, &req, sizeof(req)); });
    }

    void TFLoginFlow::SendEnterWorld(uint64_t charId)
    {
        if (!m_ctx || !m_ctx->clientNet)
            return;
        TF_EnterWorldRequest req{};
        req.charId = charId;
        DispatchAfterArmingOnboardingState(
            [&]
            {
                m_enterTimer = 0.0f;
                m_state = TFFlowState::EnteringWorld;
            },
            [&] { m_ctx->clientNet->SendMsg(TFMsg::EnterWorldReq, &req, sizeof(req)); });
    }

    // ---------------------------------------------------------------------------
    // W11 server-browser lane: join a discovered LAN server via the exact path
    // the tf_connect console command uses (TFWorldSetup::Connect).
    // ---------------------------------------------------------------------------

    void TFLoginFlow::JoinLanServer(const std::string& ip, uint16_t port)
    {
        if (!m_ctx || !m_ctx->world)
            return;
        if (m_ctx->role != NetRole::Standalone)
        {
            m_error = "Already connected - restart to join a different server.";
            return;
        }
        if (m_ctx->world->Connect(ip, port))
            m_error = "Connecting to " + ip + ":" + std::to_string(port) + " - sign in to deploy.";
        else
            m_error = "Connect to " + ip + ":" + std::to_string(port) + " failed (see log).";
    }

    // ---------------------------------------------------------------------------
    // Rendering: TFLoginFlowDraw.cpp (RenderUI + per-state screens and their
    // headless stubs).
    // ---------------------------------------------------------------------------

} // namespace Terrafront
