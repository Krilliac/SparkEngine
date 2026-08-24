/**
 * @file TFSocialSystem.cpp
 * @brief Friends / block list / recent players + online-player roster:
 *        lifecycle, client-facing API, console commands and debug UI
 *        (see TFSocialSystem.h for the full design note). The server
 *        registry, wire sends, persistence and client mirror live in the
 *        sibling TFSocialSystem*.cpp parts; shared helpers live in
 *        TFSocialSystemInternal.h.
 */
#include "Game/TFSocialSystem.h"

#include "Game/TFSocialSystemInternal.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "Persistence/TFSavePaths.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>

namespace Terrafront
{

    using namespace SocialDetail;

    namespace
    {

        constexpr float kSocialPollPeriodSec = 0.5f; // enter/leave detection cadence

    } // namespace

    TFSocialSystem::TFSocialSystem() = default;
    TFSocialSystem::~TFSocialSystem()
    {
        if (m_initialized && !Shutdown())
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] social system destroyed after its final persistence flush failed");
    }

    bool TFSocialSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_storePath = SavePaths::File("terrafront_social.json");
        if (m_storePath.empty())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] social store path is invalid");
            return false;
        }
        if (ctx.IsAuthority())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(m_storePath.parent_path(), directoryError);
            if (directoryError)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] social store directory creation failed: %s",
                                directoryError.message().c_str());
                return false;
            }
            std::error_code lockError;
            if (!m_storeLock.TryLock(m_storePath, lockError))
            {
                SPARK_LOG_ERROR(
                    Spark::LogCategory::Game,
                    "[TF] social store open refused: another authority owns the persistence lock for %s (%s)",
                    SavePaths::Utf8ForLog(m_storePath).c_str(), lockError.message().c_str());
                return false;
            }
        }

        // Recent-players sources (authority only; guarded inside the handlers).
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnBusPlayerKilled(ev); });
        events.Subscribe<EvSquadChanged>([this](const EvSquadChanged& ev) { OnBusSquadChanged(ev); });

        RegisterConsoleCommands();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSocialSystem initialized");
        return true;
    }

    void TFSocialSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        if (m_ctx->IsAuthority())
        {
            m_pollAccum += deltaTime;
            if (m_pollAccum >= kSocialPollPeriodSec)
            {
                m_pollAccum = 0.0f;
                ServerPollEnteredPlayers();
            }
            StoreFlushIfDue(deltaTime);
#ifdef ENABLE_NETWORKING
            if (ServerNetActive() && !m_serverHandlers)
                EnsureServerHandlers();
#endif
        }

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle (TFSquadSystem pattern: register AFTER
        // link-up so ours wins the per-type handler slot).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            ResetMirror();
        }
#endif
    }

    void TFSocialSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    bool TFSocialSystem::Checkpoint()
    {
        if (!m_initialized || !m_storeDirty)
            return true;
        if (!StoreSaveToDisk())
            return false;
        m_storeDirty = false;
        m_saveAccum = 0.0f;
        return true;
    }

    bool TFSocialSystem::Shutdown()
    {
        if (!m_initialized)
            return true;

        // Do not dismantle handlers or discard the in-memory store until its
        // dirty state is durable. A caller may fix the underlying I/O problem
        // and call Shutdown() again.
        if (!Checkpoint())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] social shutdown refused: persistence flush failed; state remains initialized");
            return false;
        }
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
        if (m_serverHandlers)
            ReleaseServerHandlers();
#endif
        if (m_friendCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_friend");
            m_friendCmd = false;
        }
        if (m_blockCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_block");
            m_blockCmd = false;
        }
        m_store.clear();
        m_online.clear();
        ResetMirror();
        m_storeLoaded = false;
        m_storeLoadFailed = false;
        m_saveAccum = 0.0f;
        m_pollAccum = 0.0f;
        m_initialized = false;
        m_storeLock.Unlock();
        return true;
    }

    // ---------------------------------------------------------------------------
    // Client API
    // ---------------------------------------------------------------------------

    void TFSocialSystem::ClientSendOp(SocialOp op, const std::string& targetName)
    {
        if (!m_initialized || !m_ctx || m_ctx->localPlayer == kInvalidPlayer)
            return;

        TF_SocialOp msg{};
        msg.op = static_cast<uint8_t>(op);
        CopyName(msg.targetName, TrimmedName(targetName));

        if (m_ctx->IsAuthority())
        {
            // Mirror the enter-world gate networked TF_SocialOp traffic is held to
            // (TFSquadSystem::SendOp precedent). The gate only exists under
            // ENABLE_NETWORKING — without it, onboarding is inert and social
            // features stay dormant by design.
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
            ServerHandleSocialOp(m_ctx->localPlayer, msg);
#endif
        }
        else if (m_ctx->clientNet)
        {
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFSocialMsg_Op), &msg, sizeof(msg));
        }
    }

    bool TFSocialSystem::NameOfPlayer(PlayerId player, std::string& out) const
    {
        auto it = m_roster.find(player);
        if (it == m_roster.end())
            return false;
        out = it->second.name;
        return true;
    }

    PlayerId TFSocialSystem::PlayerIdByName(const std::string& name) const
    {
        for (const auto& [id, view] : m_roster)
        {
            if (NameEq(view.name, name))
                return id;
        }
        return kInvalidPlayer;
    }

    FactionId TFSocialSystem::RosterFactionOf(PlayerId player) const
    {
        auto it = m_roster.find(player);
        return it == m_roster.end() ? FactionId::None : it->second.faction;
    }

    bool TFSocialSystem::IsBlockedName(const std::string& name) const
    {
        return std::any_of(m_blocked.begin(), m_blocked.end(),
                           [&](const EntryView& e) { return NameEq(e.name, name); });
    }

    bool TFSocialSystem::IsBlockedPlayer(PlayerId player) const
    {
        auto it = m_roster.find(player);
        return it != m_roster.end() && IsBlockedName(it->second.name);
    }

    bool TFSocialSystem::IsFriendName(const std::string& name) const
    {
        return std::any_of(m_friends.begin(), m_friends.end(),
                           [&](const EntryView& e) { return NameEq(e.name, name); });
    }

    std::vector<std::string> TFSocialSystem::DrainFriendNotices()
    {
        std::vector<std::string> out;
        out.swap(m_friendNotices);
        return out;
    }

    // ---------------------------------------------------------------------------
    // Console commands (registered from Initialize — TFRegionSystem pattern)
    // ---------------------------------------------------------------------------

    void TFSocialSystem::RegisterConsoleCommands()
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        auto joinedName = [](const std::vector<std::string>& args, size_t firstIndex) -> std::string
        {
            std::string name;
            for (size_t i = firstIndex; i < args.size(); ++i)
            {
                if (!name.empty())
                    name += ' ';
                name += args[i];
            }
            return name;
        };

        if (!console.HasCommand("tf_friend"))
        {
            console.RegisterCommand(
                "tf_friend",
                [this, joinedName](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] social system not ready";
                    if (args.size() < 2)
                        return "usage: tf_friend <add|remove> <character name>";
                    const std::string name = joinedName(args, 1);
                    if (args[0] == "add")
                        ClientSendOp(SocialOp::FriendAdd, name);
                    else if (args[0] == "remove")
                        ClientSendOp(SocialOp::FriendRemove, name);
                    else
                        return "usage: tf_friend <add|remove> <character name>";
                    return "[TF] friend " + args[0] + " '" + name + "' requested";
                },
                "Add or remove a friend by character name", "TERRAFRONT", "tf_friend <add|remove> <name>");
            m_friendCmd = true;
        }

        if (!console.HasCommand("tf_block"))
        {
            console.RegisterCommand(
                "tf_block",
                [this, joinedName](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] social system not ready";
                    if (args.size() < 2)
                        return "usage: tf_block <add|remove> <character name>";
                    const std::string name = joinedName(args, 1);
                    if (args[0] == "add")
                        ClientSendOp(SocialOp::BlockAdd, name);
                    else if (args[0] == "remove")
                        ClientSendOp(SocialOp::BlockRemove, name);
                    else
                        return "usage: tf_block <add|remove> <character name>";
                    return "[TF] block " + args[0] + " '" + name + "' requested";
                },
                "Block or unblock a character by name (blocked players' chat is hidden)", "TERRAFRONT",
                "tf_block <add|remove> <name>");
            m_blockCmd = true;
        }
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFSocialSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Social"))
            return;

        ImGui::Text("roster: %zu online   friends %zu   blocked %zu   recent %zu", m_roster.size(), m_friends.size(),
                    m_blocked.size(), m_recent.size());
        for (const auto& [id, view] : m_roster)
            ImGui::Text("  p%u '%s' [%s]", id, view.name.c_str(), FactionTag(view.faction));

        if (m_ctx && m_ctx->IsAuthority())
        {
            ImGui::Separator();
            ImGui::Text("server store: %zu character record(s)%s%s", m_store.size(), m_storeLoaded ? " (loaded)" : "",
                        m_storeDirty ? " (dirty)" : "");
            ImGui::Text("server online: %zu", m_online.size());
        }
#endif
    }

} // namespace Terrafront
