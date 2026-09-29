/** @file MMOSessionCommands.cpp
 * @brief Interactive client commands for the production MMO session gate.
 */
#include "SparkGameMMO.h"

#ifdef ENABLE_NETWORKING

#include "Player/MMOPlayerSystem.h"
#include "Session/MMOSessionGate.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <vector>

namespace
{
    bool ParseSessionUint(const std::string& text, uint32_t& value)
    {
        if (text.empty())
        {
            return false;
        }
        const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} && ptr == text.data() + text.size();
    }
} // namespace

void SparkGameMMOModule::RegisterSessionConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.RegisterCommand(
        "mmo_session_create",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
            {
                return "Usage: mmo_session_create <name> <race> <class>";
            }
            if (!m_sessionGate)
            {
                return "Session gate unavailable";
            }
            uint32_t race = 0;
            uint32_t classId = 0;
            if (!ParseSessionUint(args[1], race) || !ParseSessionUint(args[2], classId) || race > 255 || classId > 255)
            {
                return "Invalid race or class";
            }
            MMO::SessionGateWire::Packet packet;
            packet.operation = MMO::SessionGateWire::Operation::CreateCharacter;
            packet.race = static_cast<uint8_t>(race);
            packet.classId = static_cast<uint8_t>(classId);
            if (args[0].size() >= packet.name.size())
            {
                return "Character name too long";
            }
            std::copy_n(args[0].data(), args[0].size(), packet.name.data());
            return m_sessionGate->Send(packet) ? "Character creation sent" : "Character creation unavailable";
        });

    console.RegisterCommand("mmo_session_enter",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                {
                                    return "Usage: mmo_session_enter <character_id>";
                                }
                                if (!m_sessionGate)
                                {
                                    return "Session gate unavailable";
                                }
                                uint32_t characterId = 0;
                                if (!ParseSessionUint(args[0], characterId))
                                {
                                    return "Invalid character ID";
                                }
                                MMO::SessionGateWire::Packet packet;
                                packet.operation = MMO::SessionGateWire::Operation::EnterWorld;
                                packet.characterId = characterId;
                                return m_sessionGate->Send(packet) ? "World entry sent" : "World entry unavailable";
                            });

    console.RegisterCommand("mmo_session_interact",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (args.empty())
                                {
                                    return "Usage: mmo_session_interact <character_id>";
                                }
                                if (!m_sessionGate)
                                {
                                    return "Session gate unavailable";
                                }
                                uint32_t characterId = 0;
                                if (!ParseSessionUint(args[0], characterId))
                                {
                                    return "Invalid character ID";
                                }
                                const auto* local = m_playerSystem ? m_playerSystem->GetLocalPlayer() : nullptr;
                                if (!local || local->characterId == 0)
                                {
                                    return "Enter the world first";
                                }
                                MMO::SessionGateWire::Packet packet;
                                packet.operation = MMO::SessionGateWire::Operation::Interact;
                                packet.characterId = local->characterId;
                                packet.targetId = characterId;
                                return m_sessionGate->Send(packet) ? "Interaction sent" : "Interaction unavailable";
                            });

    console.RegisterCommand("mmo_session_status",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (!m_sessionGate)
                                {
                                    return "Session gate unavailable";
                                }
                                const auto& reply = m_sessionGate->GetLastReply();
                                return "op=" + std::to_string(static_cast<int>(reply.operation)) +
                                       " status=" + std::to_string(static_cast<int>(reply.status)) +
                                       " request=" + std::to_string(reply.requestId) +
                                       " account=" + std::to_string(reply.accountId) +
                                       " character=" + std::to_string(reply.characterId);
                            });
}
#endif
