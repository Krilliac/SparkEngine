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
#include <exception>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

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

    struct Options
    {
        std::string role;
        uint16_t port = 0;
        std::string key;
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
            if (!gate.Send(packet) || !PumpUntilReply(network, gate, packet.requestId, std::chrono::seconds(12)))
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

    int RunServer()
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
        if (!InitializeSystems(network, context, gate, accounts, characters, players))
        {
            return 3;
        }
        Emit(std::format("ready port={} key={}", network.GetBoundPort(),
                         EncodeKey(network.GetSecurityConfig().identity->publicKey)));

        const auto quit = std::make_shared<std::atomic<bool>>(false);
        std::thread stdinThread(
            [quit]
            {
                std::string command;
                while (std::getline(std::cin, command))
                {
                    if (command == "quit")
                    {
                        quit->store(true);
                        return;
                    }
                }
                quit->store(true);
            });
        auto previous = Clock::now();
        while (!quit->load())
        {
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

        const auto connectionDeadline = Clock::now() + std::chrono::seconds(12);
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
            const auto stateDeadline = Clock::now() + std::chrono::seconds(12);
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
    return options->role == "server" ? RunServer() : RunClient(*options);
}
