/**
 * @file SessionGatePeer.cpp
 * @brief MOD-320 real-process peer for the MMO session-gate loopback test.
 *
 * The peer is deliberately a thin process wrapper.  Authentication, character
 * ownership, world entry, movement, interaction, and wire dispatch all remain
 * in MMO::MMOSessionGate; this file only supplies real NetworkManager lifecycle
 * and a deterministic coordinator protocol.
 */

#include "Session/MMOSessionGate.h"
#include "Account/MMOAccountSystem.h"
#include "Character/MMOCharacterSystem.h"
#include "Player/MMOPlayerSystem.h"
#include "Persistence/MMOPersistenceSystem.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/NetworkTrustStore.h"
#include "Core/EngineContext.h"
#include "Utils/SecureMemory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    using Packet = MMO::SessionGateWire::Packet;
    using Operation = MMO::SessionGateWire::Operation;
    using Status = MMO::SessionGateWire::Status;
    using Clock = std::chrono::steady_clock;

    constexpr std::string_view kPrefix = "SESSION_GATE ";
    constexpr std::string_view kAliceUser = "mod320_alice";
    constexpr std::string_view kBobUser = "mod320_bob";
    constexpr std::string_view kAlicePassword = "mod320-alice-password";
    constexpr std::string_view kBobPassword = "mod320-bob-password";

    // Upper bounds, not expected durations: every wait polls for readiness and returns as soon as the
    // reply or state arrives. They are meant to tolerate a loaded host (three processes sharing a
    // 600000-round PBKDF2); that is untested under load, and the RateLimited retry cap is a fixed 20 attempts.
    // The coordinator enforces the overall limit.
    constexpr auto kConnectTimeout = std::chrono::seconds(60);
    constexpr auto kReplyTimeout = std::chrono::seconds(60);
    constexpr auto kStateTimeout = std::chrono::seconds(60);

    struct Options
    {
        std::string role;
        uint16_t port = 0;
        std::string key;
        std::string store; ///< server only: durable MMOPersistenceSystem file shared across restarts
    };

    void Emit(std::string_view body)
    {
        std::cout << kPrefix << body << '\n' << std::flush;
    }

    bool ParseUInt(std::string_view text, uint32_t& value)
    {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} && end == text.data() + text.size();
    }

    std::optional<Options> ParseOptions(int argc, char** argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            if (index + 1 >= argc)
            {
                return std::nullopt;
            }
            const std::string_view flag = argv[index];
            const std::string_view value = argv[++index];
            if (flag == "--role")
            {
                options.role = value;
            }
            else if (flag == "--port")
            {
                uint32_t parsed = 0;
                if (!ParseUInt(value, parsed) || parsed == 0 || parsed > 65535)
                {
                    return std::nullopt;
                }
                options.port = static_cast<uint16_t>(parsed);
            }
            else if (flag == "--server-key")
            {
                options.key = value;
            }
            else if (flag == "--store")
            {
                options.store = value;
            }
            else
            {
                return std::nullopt;
            }
        }
        if (options.role != "server" && options.role != "alice" && options.role != "bob")
        {
            return std::nullopt;
        }
        if (options.role == "server")
        {
            return options;
        }
        if (!options.store.empty())
        {
            return std::nullopt;
        }
        return options.port != 0 && options.key.size() == Spark::Net::HANDSHAKE_PUBLIC_KEY_SIZE * 2
                   ? std::optional<Options>(std::move(options))
                   : std::nullopt;
    }

    bool DecodeKey(std::string_view text, Spark::Net::ServerPublicKey& key)
    {
        if (text.size() != key.size() * 2)
        {
            return false;
        }
        for (size_t index = 0; index < key.size(); ++index)
        {
            unsigned value = 0;
            const auto [end, error] = std::from_chars(text.data() + index * 2, text.data() + index * 2 + 2, value, 16);
            if (error != std::errc{} || end != text.data() + index * 2 + 2)
            {
                return false;
            }
            key[index] = static_cast<uint8_t>(value);
        }
        return true;
    }

    std::string EncodeKey(const Spark::Net::ServerPublicKey& key)
    {
        std::string result;
        result.reserve(key.size() * 2);
        for (const uint8_t byte : key)
        {
            result += std::format("{:02x}", byte);
        }
        return result;
    }

    void Put(std::array<char, 33>& destination, std::string_view text)
    {
        destination.fill('\0');
        std::memcpy(destination.data(), text.data(), (std::min)(text.size(), destination.size() - 1));
    }

    void Put(std::array<char, 129>& destination, std::string_view text)
    {
        destination.fill('\0');
        std::memcpy(destination.data(), text.data(), (std::min)(text.size(), destination.size() - 1));
    }

    void Put(std::array<char, 17>& destination, std::string_view text)
    {
        destination.fill('\0');
        std::memcpy(destination.data(), text.data(), (std::min)(text.size(), destination.size() - 1));
    }

    /**
     * Wall-clock frame delta, clamped like the server loop. NetworkManager measures its
     * server-silence timeout in Update() time, so a fixed 1/60 s per 2 ms poll ran the
     * client clock ~7x fast and dropped the session whenever the server spent a few real
     * seconds in PBKDF (Debug builds), before the reply could arrive.
     */
    float FrameDelta(Clock::time_point& previous)
    {
        const auto now = Clock::now();
        const float delta = (std::min)(std::chrono::duration<float>(now - previous).count(), 0.1F);
        previous = now;
        return delta;
    }

    bool PumpUntilReply(Spark::Net::NetworkManager& network, MMO::MMOSessionGate& gate, uint32_t requestId,
                        std::chrono::milliseconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        auto previous = Clock::now();
        while (Clock::now() < deadline)
        {
            const float delta = FrameDelta(previous);
            network.Update(delta);
            gate.Update(delta);
            if (gate.GetLastReply().requestId == requestId && gate.GetLastReply().response)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    bool SendRequest(Spark::Net::NetworkManager& network, MMO::MMOSessionGate& gate, Packet packet, Status expected)
    {
        // Both clients share the production admission budget. A rate refusal is retried
        // with a new sequence, never treated as the expected authentication refusal.
        for (unsigned attempt = 0; attempt < 20; ++attempt)
        {
            packet.requestId = gate.GetLastReply().requestId + 1;
            if (!gate.Send(packet) || !PumpUntilReply(network, gate, packet.requestId, kReplyTimeout))
            {
                Spark::SecureErase(packet.password.data(), packet.password.size());
                // Diagnostics only: operation and outcome, never the credential.
                Emit(std::format("request-failed op={} reason=no-reply connected={}",
                                 static_cast<unsigned>(packet.operation),
                                 network.GetConnectionState() == Spark::Net::ConnectionState::Connected ? 1 : 0));
                return false;
            }
            const Status status = gate.GetLastReply().status;
            if (status != Status::RateLimited)
            {
                Spark::SecureErase(packet.password.data(), packet.password.size());
                if (status != expected)
                {
                    Emit(std::format("request-failed op={} status={} expected={}",
                                     static_cast<unsigned>(packet.operation), static_cast<unsigned>(status),
                                     static_cast<unsigned>(expected)));
                }
                return status == expected;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(275));
        }
        Spark::SecureErase(packet.password.data(), packet.password.size());
        return false;
    }

    bool InitializeSystems(Spark::Net::NetworkManager& network, EngineContext& context, MMO::MMOSessionGate& gate,
                           MMO::MMOAccountSystem& accounts, MMO::MMOCharacterSystem& characters,
                           MMO::MMOPlayerSystem& players)
    {
        if (!accounts.Initialize(&context) || !characters.Initialize(&context) || !players.Initialize(&context))
        {
            return false;
        }
        return gate.Initialize(network, accounts, characters, players);
    }

    /// Thread-safe line queue: the stdin thread produces, the server loop consumes on the game thread.
    struct CommandQueue
    {
        std::mutex mutex;
        std::deque<std::string> pending;
        std::atomic<bool> quit{false};

        void Push(std::string command)
        {
            const std::lock_guard<std::mutex> lock(mutex);
            pending.push_back(std::move(command));
        }

        std::optional<std::string> Pop()
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (pending.empty())
            {
                return std::nullopt;
            }
            std::string command = std::move(pending.front());
            pending.pop_front();
            return command;
        }
    };

    std::vector<std::string_view> Tokens(const std::string& line)
    {
        std::vector<std::string_view> tokens;
        size_t start = 0;
        while (start < line.size())
        {
            const size_t end = line.find(' ', start);
            const size_t stop = end == std::string::npos ? line.size() : end;
            if (stop > start)
            {
                tokens.emplace_back(line.data() + start, stop - start);
            }
            start = stop + 1;
        }
        return tokens;
    }

    /// The authoritative actor of a character, wherever the gate placed its client.
    const MMO::MMOPlayer* FindActor(const MMO::MMOPlayerSystem& players, uint32_t characterId)
    {
        for (uint32_t clientId = 1; clientId <= 256; ++clientId)
        {
            const MMO::MMOPlayer* actor = players.GetPlayer(clientId);
            if (actor && actor->characterId == characterId)
            {
                return actor;
            }
        }
        return nullptr;
    }

    /**
     * Store commands. They only call production persistence over production server state:
     *   save <account> <character>   write the owned character with its live authoritative position
     *   saveworld <leader> <member>  write one guild of two stored characters
     *   load <character>             read the character row back
     *   loadworld                    read the guilds back
     *   alloc                        reserve the next durable character id
     */
    void HandleStoreCommand(const std::string& line, MMO::MMOPersistenceSystem* store,
                            const MMO::MMOCharacterSystem& characters, const MMO::MMOPlayerSystem& players)
    {
        const std::vector<std::string_view> words = Tokens(line);
        if (words.empty())
        {
            return;
        }
        std::array<uint32_t, 2> numbers{};
        for (size_t index = 1; index < words.size() && index <= numbers.size(); ++index)
        {
            if (!ParseUInt(words[index], numbers[index - 1]))
            {
                Emit(std::format("store-failed command={} reason=bad-argument", words[0]));
                return;
            }
        }
        if (!store || !store->IsInitialized())
        {
            Emit(std::format("store-failed command={} reason=no-store", words[0]));
            return;
        }

        if (words[0] == "save" && words.size() == 3)
        {
            const uint32_t accountId = numbers[0];
            const uint32_t characterId = numbers[1];
            const MMO::CharacterSummary* summary = characters.GetCharacter(characterId);
            const MMO::MMOPlayer* actor = FindActor(players, characterId);
            if (!summary || !actor || !characters.OwnsCharacter(accountId, characterId))
            {
                Emit(std::format("store-failed command=save character={} reason=unknown-character", characterId));
                return;
            }
            MMO::CharacterSaveData data;
            data.characterId = characterId;
            data.accountId = accountId;
            data.name = summary->name;
            data.level = summary->level;
            data.areaId = summary->areaId;
            data.posX = actor->posX;
            data.posY = actor->posY;
            data.posZ = actor->posZ;
            if (!store->SaveCharacterSync(data))
            {
                Emit(std::format("store-failed command=save character={} reason=write", characterId));
                return;
            }
            Emit(std::format("saved character={} account={} x={:.3f} z={:.3f}", characterId, accountId, data.posX,
                             data.posZ));
        }
        else if (words[0] == "saveworld" && words.size() == 3)
        {
            const MMO::CharacterSummary* leader = characters.GetCharacter(numbers[0]);
            const MMO::CharacterSummary* member = characters.GetCharacter(numbers[1]);
            if (!leader || !member)
            {
                Emit("store-failed command=saveworld reason=unknown-character");
                return;
            }
            MMO::Guild guild;
            guild.id = 1;
            guild.name = "Mod320Guild";
            guild.tag = "M320";
            guild.motd = "survives a process kill";
            guild.leaderId = leader->characterId;
            guild.bankCurrency = 320;
            guild.members.push_back(MMO::GuildMember{.playerId = leader->characterId,
                                                     .name = leader->name,
                                                     .rank = MMO::GuildRank::Leader,
                                                     .level = 1,
                                                     .isOnline = false,
                                                     .contribution = 0.0F,
                                                     .note = {}});
            guild.members.push_back(MMO::GuildMember{.playerId = member->characterId,
                                                     .name = member->name,
                                                     .rank = MMO::GuildRank::Member,
                                                     .level = 1,
                                                     .isOnline = false,
                                                     .contribution = 0.0F,
                                                     .note = {}});
            MMO::WorldSaveData world;
            world.guilds.push_back(std::move(guild));
            world.nextGuildId = 2;
            if (!store->SaveWorldSync(world))
            {
                Emit("store-failed command=saveworld reason=write");
                return;
            }
            Emit("worldsaved guilds=1");
        }
        else if (words[0] == "load" && words.size() == 2)
        {
            MMO::CharacterSaveData data;
            if (!store->LoadCharacter(numbers[0], data))
            {
                Emit(std::format("store-failed command=load character={} reason=missing", numbers[0]));
                return;
            }
            Emit(std::format("loaded character={} account={} name={} x={:.3f} z={:.3f}", data.characterId,
                             data.accountId, data.name, data.posX, data.posZ));
        }
        else if (words[0] == "loadworld" && words.size() == 1)
        {
            MMO::WorldSaveData world;
            if (!store->LoadWorld(world) || world.guilds.size() != 1)
            {
                Emit("store-failed command=loadworld reason=read");
                return;
            }
            const MMO::Guild& guild = world.guilds.front();
            Emit(std::format("worldloaded guilds={} name={} leader={} bank={} members={} nextGuildId={}",
                             world.guilds.size(), guild.name, guild.leaderId, guild.bankCurrency, guild.members.size(),
                             world.nextGuildId));
        }
        else if (words[0] == "alloc" && words.size() == 1)
        {
            Emit(std::format("allocated character={}", store->AllocateCharacterId()));
        }
        else
        {
            Emit(std::format("store-failed command={} reason=unknown-command", words[0]));
        }
    }

    int RunServer(const Options& options)
    {
        auto& network = Spark::Net::NetworkManager::GetInstance();
        auto identity = Spark::Net::GenerateServerIdentity();
        if (!identity)
        {
            return 2;
        }
        Spark::Net::NetworkSecurityConfig security;
        security.identity = std::move(*identity);
        network.SetSecurityConfig(std::move(security));
        if (!network.Initialize() || !network.StartServer(0, 8))
        {
            return 2;
        }

        EngineContext context;
        context.SetNetwork(&network);
        MMO::MMOAccountSystem accounts;
        MMO::MMOCharacterSystem characters;
        MMO::MMOPlayerSystem players;
        MMO::MMOSessionGate gate;
        MMO::MMOPersistenceSystem store;
        if (!InitializeSystems(network, context, gate, accounts, characters, players))
        {
            return 3;
        }
        if (!options.store.empty())
        {
            // Character IDs now come from the store's durable counter, as in the production module.
            if (!store.Initialize(&context, options.store))
            {
                return 5;
            }
            characters.SetPersistence(&store);
        }
        Emit(std::format("ready port={} key={}", network.GetBoundPort(),
                         EncodeKey(network.GetSecurityConfig().identity->publicKey)));

        const auto commands = std::make_shared<CommandQueue>();
        std::thread stdinThread(
            [commands]
            {
                std::string command;
                while (std::getline(std::cin, command))
                {
                    if (command == "quit")
                    {
                        commands->quit.store(true);
                        return;
                    }
                    commands->Push(std::move(command));
                }
                commands->quit.store(true);
            });
        auto previous = Clock::now();
        while (!commands->quit.load())
        {
            while (const auto command = commands->Pop())
            {
                HandleStoreCommand(*command, options.store.empty() ? nullptr : &store, characters, players);
            }
            const auto now = Clock::now();
            const float delta = (std::min)(std::chrono::duration<float>(now - previous).count(), 0.1F);
            previous = now;
            network.Update(delta);
            gate.Update(delta);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        stdinThread.detach();
        gate.Shutdown();
        players.Shutdown();
        characters.Shutdown();
        accounts.Shutdown();
        store.Shutdown();
        network.StopServer();
        network.Shutdown();
        return 0;
    }

    int RunClient(const Options& options)
    {
        Spark::Net::ServerPublicKey key{};
        if (!DecodeKey(options.key, key))
        {
            return 1;
        }
        auto& network = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::NetworkSecurityConfig security;
        security.trust = Spark::Net::ServerTrust::Pin(key);
        network.SetSecurityConfig(std::move(security));
        if (!network.Initialize() || !network.Connect("127.0.0.1", options.port, options.role))
        {
            return 2;
        }

        const auto connectionDeadline = Clock::now() + kConnectTimeout;
        auto connectPrevious = Clock::now();
        while (network.GetConnectionState() != Spark::Net::ConnectionState::Connected &&
               Clock::now() < connectionDeadline)
        {
            network.Update(FrameDelta(connectPrevious));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (network.GetConnectionState() != Spark::Net::ConnectionState::Connected)
        {
            return 3;
        }

        EngineContext context;
        context.SetNetwork(&network);
        MMO::MMOAccountSystem accounts;
        MMO::MMOCharacterSystem characters;
        MMO::MMOPlayerSystem players;
        MMO::MMOSessionGate gate;
        if (!InitializeSystems(network, context, gate, accounts, characters, players))
        {
            return 4;
        }

        const bool alice = options.role == "alice";
        const std::string_view username = alice ? kAliceUser : kBobUser;
        const std::string_view password = alice ? kAlicePassword : kBobPassword;
        uint32_t requestId = 1;
        Packet packet{};
        packet.operation = Operation::Register;
        packet.requestId = requestId++;
        Put(packet.username, username);
        Put(packet.password, password);
        if (!SendRequest(network, gate, packet, Status::Ok))
        {
            return 5;
        }
        uint32_t accountId = 0;

        if (!alice)
        {
            Packet bad{};
            bad.operation = Operation::Login;
            bad.requestId = requestId++;
            Put(bad.username, username);
            Put(bad.password, "mod320-wrong-password");
            if (!SendRequest(network, gate, bad, Status::Rejected))
            {
                return 6;
            }
            Emit("negative bad-password=refused");
        }

        Packet unauthenticated{};
        unauthenticated.operation = Operation::CreateCharacter;
        unauthenticated.requestId = requestId++;
        Put(unauthenticated.name, alice ? "Alice" : "Bob");
        if (!SendRequest(network, gate, unauthenticated, Status::Unauthenticated))
        {
            return 17;
        }
        unauthenticated = {};
        unauthenticated.operation = Operation::EnterWorld;
        unauthenticated.requestId = requestId++;
        unauthenticated.characterId = 1;
        if (!SendRequest(network, gate, unauthenticated, Status::Unauthenticated))
        {
            return 18;
        }
        Emit("negative unauthenticated=refused");

        packet = {};
        packet.operation = Operation::Login;
        packet.requestId = requestId++;
        Put(packet.username, username);
        Put(packet.password, password);
        if (!SendRequest(network, gate, packet, Status::Ok))
        {
            return 7;
        }
        accountId = gate.GetLastReply().accountId;
        if (accountId == 0)
        {
            return 16;
        }

        packet = {};
        packet.operation = Operation::CreateCharacter;
        packet.requestId = requestId++;
        packet.accountId = accountId;
        Put(packet.name, alice ? "Alice" : "Bob");
        packet.race = 0;
        packet.classId = 0;
        if (!SendRequest(network, gate, packet, Status::Ok))
        {
            return 8;
        }
        const uint32_t characterId = gate.GetLastReply().characterId;

        packet = {};
        packet.operation = Operation::EnterWorld;
        packet.requestId = requestId++;
        packet.accountId = accountId;
        packet.characterId = characterId;
        if (!SendRequest(network, gate, packet, Status::Ok))
        {
            return 9;
        }
        Emit(std::format("ready role={} account={} character={}", options.role, accountId, characterId));

        std::string command;
        while (std::getline(std::cin, command) && command != "quit")
        {
            if (command.rfind("act ", 0) != 0)
            {
                continue;
            }
            uint32_t targetCharacter = 0;
            if (!ParseUInt(command.substr(4), targetCharacter))
            {
                return 10;
            }
            if (!alice)
            {
                Packet unauthorized{};
                unauthorized.operation = Operation::EnterWorld;
                unauthorized.requestId = requestId++;
                unauthorized.accountId = accountId;
                unauthorized.characterId = targetCharacter;
                if (!SendRequest(network, gate, unauthorized, Status::NotOwner))
                {
                    return 13;
                }
                unauthorized = {};
                unauthorized.operation = Operation::Move;
                unauthorized.requestId = requestId++;
                unauthorized.accountId = accountId;
                unauthorized.characterId = targetCharacter;
                unauthorized.x = 1.0f;
                if (!SendRequest(network, gate, unauthorized, Status::NotOwner))
                {
                    return 14;
                }
                unauthorized = {};
                unauthorized.operation = Operation::Interact;
                unauthorized.requestId = requestId++;
                unauthorized.accountId = accountId;
                unauthorized.characterId = targetCharacter;
                unauthorized.targetId = characterId;
                if (!SendRequest(network, gate, unauthorized, Status::NotOwner))
                {
                    return 15;
                }
                Emit("negative ownership=refused");
            }
            for (uint32_t step = 0; step < 10; ++step)
            {
                packet = {};
                packet.operation = Operation::Move;
                packet.requestId = requestId++;
                packet.accountId = accountId;
                packet.characterId = characterId;
                packet.x = 1.0f;
                if (!SendRequest(network, gate, packet, Status::Ok))
                {
                    return 11;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(17));
            }
            packet = {};
            packet.operation = Operation::Interact;
            packet.requestId = requestId++;
            packet.accountId = accountId;
            packet.characterId = characterId;
            packet.targetId = targetCharacter;
            if (!SendRequest(network, gate, packet, Status::Ok))
            {
                return 12;
            }
            const auto stateDeadline = Clock::now() + kStateTimeout;
            const Packet* ownState = nullptr;
            const Packet* targetState = nullptr;
            auto statePrevious = Clock::now();
            while (Clock::now() < stateDeadline)
            {
                const float delta = FrameDelta(statePrevious);
                network.Update(delta);
                gate.Update(delta);
                ownState = gate.GetState(characterId);
                targetState = gate.GetState(targetCharacter);
                if (ownState && targetState && ownState->interactionCount == 1 &&
                    ownState->targetId == targetCharacter && targetState->interactionCount == 1 &&
                    targetState->targetId == characterId)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!ownState || !targetState || ownState->interactionCount != 1 || ownState->targetId != targetCharacter ||
                targetState->interactionCount != 1 || targetState->targetId != characterId ||
                std::abs(ownState->x - 1.0f) > 0.001f || std::abs(targetState->x - 1.0f) > 0.001f ||
                std::abs(ownState->z) > 0.001f || std::abs(targetState->z) > 0.001f)
            {
                return 19;
            }
            Emit(std::format(
                "done role={} character={} target={} interactions={} observed={} observedX={:.3f} observedZ={:.3f} "
                "ownX={:.3f} observedInteractions={} observedTarget={}",
                options.role, characterId, targetCharacter, ownState->interactionCount, targetState->characterId,
                targetState->x, targetState->z, ownState->x, targetState->interactionCount, targetState->targetId));
        }
        gate.Shutdown();
        players.Shutdown();
        characters.Shutdown();
        accounts.Shutdown();
        network.Disconnect();
        network.Shutdown();
        return 0;
    }
} // namespace

int main(int argc, char** argv)
{
    const auto options = ParseOptions(argc, argv);
    if (!options)
    {
        return 1;
    }
    return options->role == "server" ? RunServer(*options) : RunClient(*options);
}
