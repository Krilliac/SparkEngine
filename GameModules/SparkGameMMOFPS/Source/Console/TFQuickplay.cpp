/**
 * @file TFQuickplay.cpp
 * @brief Security-gated local-listen-host onboarding and war bootstrap.
 */
#include "Console/TFQuickplay.h"

#include "Account/TFAccountSystem.h"
#include "Account/TFCharacterSystem.h"
#include "Game/TFBotSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFServerSim.h"
#include "Utils/ScopeGuard.h"
#include "Utils/SecureMemory.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstring>
#include <sstream>

namespace Terrafront
{
    namespace
    {
        bool IsLocalListenHostReady(const TFGameContext& ctx)
        {
#ifdef ENABLE_NETWORKING
            const auto& network = Spark::Net::NetworkManager::GetInstance();
            const bool runtimeReady = IsQuickplayListenHostRuntime(
                ctx.role, network.IsInitialized(), network.GetRole() == Spark::Net::NetworkRole::Server,
                network.GetConnectionState() == Spark::Net::ConnectionState::Connected);
            return runtimeReady && ctx.clientNet && ctx.clientNet->IsConnected() && ctx.serverSim && ctx.players;
#else
            (void)ctx;
            return false;
#endif
        }
    } // namespace

    std::string RunLocalQuickplay(TFGameContext& ctx, TFBotSystem* bots, const std::vector<std::string>& args)
    {
        TFQuickplayOptions options;
        std::string parseError;
        if (!ParseQuickplayOptions(args, options, parseError))
            return "[TF] " + parseError;
        const auto clearPassword = Spark::MakeScopeExit([&] { Spark::SecureClear(options.password); });
        if (!IsLocalListenHostReady(ctx))
            return "[TF] quickplay is local listen-host only; run tf_host first";
        if (options.username.size() >= sizeof(TF_AuthRequest{}.user) ||
            options.password.size() >= sizeof(TF_AuthRequest{}.pass))
            return "[TF] quickplay username/password exceeds the onboarding wire limit";

        const PlayerId player = ctx.clientNet->LocalPlayerId();
        if (player == kInvalidPlayer)
            return "[TF] quickplay local identity is not ready yet; retry in a moment";
        if (ctx.InWorld())
            return "[TF] quickplay refused: a character is already active; run tf_disconnect before replaying";
        if (ctx.clientNet->IsLoggedIn())
            return "[TF] quickplay refused: an authenticated session is already active; run tf_disconnect before "
                   "switching profiles";

        // Every accepted invocation starts from a fresh session and authenticates
        // its supplied credentials. A cached session is rejected above rather
        // than silently applying different credentials to the old account.
        {
            TF_AuthRequest registration{};
            const auto clearRegistration =
                Spark::MakeScopeExit([&] { Spark::SecureErase(&registration, sizeof(registration)); });
            std::strncpy(registration.user, options.username.c_str(), sizeof(registration.user) - 1);
            std::strncpy(registration.pass, options.password.c_str(), sizeof(registration.pass) - 1);
            ctx.clientNet->SendMsg(TFMsg::RegisterRequest, &registration, sizeof(registration));
        }

        const auto registerError = static_cast<TFAuthErr>(ctx.clientNet->LastAuthError());
        if (registerError != TFAuthErr::Ok && registerError != TFAuthErr::UsernameTaken)
            return "[TF] quickplay registration failed: err=" + std::to_string(static_cast<int>(registerError));

        {
            TF_AuthRequest login{};
            const auto clearLogin = Spark::MakeScopeExit([&] { Spark::SecureErase(&login, sizeof(login)); });
            std::strncpy(login.user, options.username.c_str(), sizeof(login.user) - 1);
            std::strncpy(login.pass, options.password.c_str(), sizeof(login.pass) - 1);
            ctx.clientNet->SendMsg(TFMsg::LoginRequest, &login, sizeof(login));
        }
        Spark::SecureClear(options.password);
        if (!ctx.clientNet->IsLoggedIn())
            return "[TF] quickplay login failed: err=" +
                   std::to_string(static_cast<int>(ctx.clientNet->LastAuthError()));

        ctx.clientNet->SendMsg(TFMsg::CharListRequest, nullptr, 0);
        uint64_t characterId = 0;
        FactionId characterFaction = FactionId::None;
        for (const TF_CharBrief& character : ctx.clientNet->CharacterList())
        {
            if (static_cast<FactionId>(character.faction) == options.faction)
            {
                characterId = character.id;
                characterFaction = options.faction;
                break;
            }
        }

        if (characterId == 0)
        {
            TF_CharCreateRequest create{};
            const std::string name = QuickplayCharacterName(ctx.clientNet->AccountId(), options.faction);
            std::strncpy(create.name, name.c_str(), sizeof(create.name) - 1);
            create.faction = static_cast<uint8_t>(options.faction);
            ctx.clientNet->SendMsg(TFMsg::CharCreateReq, &create, sizeof(create));
            if (static_cast<TFCharErr>(ctx.clientNet->LastCharOpError()) != TFCharErr::Ok)
                return "[TF] quickplay character creation failed: err=" +
                       std::to_string(static_cast<int>(ctx.clientNet->LastCharOpError()));
            characterId = ctx.clientNet->LastCharOpId();
            characterFaction = options.faction;
        }

        if (!ctx.InWorld())
        {
            TF_EnterWorldRequest enter{};
            enter.charId = characterId;
            ctx.clientNet->SendMsg(TFMsg::EnterWorldReq, &enter, sizeof(enter));
            if (!ctx.InWorld())
                return "[TF] quickplay enter-world failed for character " + std::to_string(characterId);
        }

        ctx.localFaction = characterFaction;
        if (!ctx.serverSim->IsPlayerAlive(player))
        {
            TF_SpawnRequest spawn{};
            spawn.classId = static_cast<uint8_t>(options.playerClass);
            spawn.spawnKind = 0;
            ctx.clientNet->SendMsg(TFMsg::SpawnRequest, &spawn, sizeof(spawn));
            if (!ctx.serverSim->IsPlayerAlive(player))
                return "[TF] quickplay entered the world, but the deploy request was rejected";
        }

        if (bots)
            bots->ServerSetBotCount(options.botCount);

        std::ostringstream result;
        result << "[TF] quickplay ready: character " << characterId << ", " << options.botCount
               << " bots; WASD/mouse to fight, M map, Enter chat";
        return result.str();
    }
} // namespace Terrafront
