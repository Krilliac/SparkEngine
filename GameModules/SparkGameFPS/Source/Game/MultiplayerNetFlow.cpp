/**
 * @file MultiplayerNetFlow.cpp
 * @brief FPSMultiplayerSystem NetworkManager message flow: handlers, input and snapshot codecs, net diagnostics
 */

#include "MultiplayerSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace SparkFPS
{

    namespace
    {
        // StateSnapshot wire layout (see FPSMessageType::StateSnapshot).
        constexpr size_t kSnapshotHeaderSize = sizeof(uint32_t) + sizeof(uint16_t);
        constexpr size_t kSnapshotScoreSize = 4 * sizeof(uint32_t);
        constexpr size_t kSnapshotRecordSize = NetworkPlayerState::SerializedSize + kSnapshotScoreSize;

        bool IsFiniteState(const NetworkPlayerState& state)
        {
            return std::isfinite(state.posX) && std::isfinite(state.posY) && std::isfinite(state.posZ) &&
                   std::isfinite(state.velX) && std::isfinite(state.velY) && std::isfinite(state.velZ) &&
                   std::isfinite(state.yaw) && std::isfinite(state.pitch) && std::isfinite(state.health);
        }
    } // namespace

    // ============================================================================
    // Snapshot Broadcast
    // ============================================================================

    void FPSMultiplayerSystem::SendStateSnapshot()
    {
        ++m_stateSequence;
        for (auto& [id, state] : m_playerStates)
        {
            state.sequenceNumber = m_stateSequence;
            auto inputIt = m_lastInputByPlayer.find(id);
            if (inputIt != m_lastInputByPlayer.end())
            {
                state.acknowledgedInputSequence = inputIt->second.sequenceNumber;
            }
            auto& history = m_remoteSnapshots[id];
            history.push_back(state);
            while (history.size() > kMaxSnapshotHistory)
            {
                history.pop_front();
            }
        }

        if (m_playerStates.size() > kMaxPlayers)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "FPSMultiplayerSystem: %zu players exceed the %u-player batch",
                            m_playerStates.size(), kMaxPlayers);
            return;
        }

        std::vector<uint8_t> payload;
        payload.reserve(kSnapshotHeaderSize + m_playerStates.size() * kSnapshotRecordSize);
        Detail::WriteU32(payload, m_stateSequence);
        const auto count = static_cast<uint16_t>(m_playerStates.size());
        payload.push_back(static_cast<uint8_t>(count));
        payload.push_back(static_cast<uint8_t>(count >> 8));
        for (const auto& [id, state] : m_playerStates)
        {
            const std::vector<uint8_t> record = state.Serialize();
            payload.insert(payload.end(), record.begin(), record.end());
            const auto scoreIt = m_scores.find(id);
            const bool scored = scoreIt != m_scores.end();
            Detail::WriteU32(payload, scored ? scoreIt->second.kills : 0);
            Detail::WriteU32(payload, scored ? scoreIt->second.deaths : 0);
            Detail::WriteU32(payload, scored ? scoreIt->second.assists : 0);
            Detail::WriteU32(payload, scored ? static_cast<uint32_t>(scoreIt->second.score) : 0);
        }

        Spark::Net::NetworkMessage message;
        message.type = static_cast<Spark::Net::MessageType>(FPSMessageType::StateSnapshot);
        message.channel = Spark::Net::ChannelType::Unreliable;
        message.payload = std::move(payload);
        Spark::Net::NetworkManager::GetInstance().SendToAll(message);
    }

    // ============================================================================
    // NetworkManager Message Flow
    // ============================================================================

    void FPSMultiplayerSystem::RegisterNetworkHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& network = Spark::Net::NetworkManager::GetInstance();

        // NetworkManager surfaces each admitted client once as a Connect event whose
        // senderID is the id it assigned.
        network.RegisterHandler(MessageType::Connect,
                                [this](const NetworkMessage& message)
                                {
                                    if (m_isServer && m_isActive && message.senderID != Spark::Net::INVALID_CLIENT &&
                                        !m_playerStates.contains(message.senderID))
                                    {
                                        OnPlayerJoined(message.senderID);
                                    }
                                });
        network.RegisterHandler(MessageType::Disconnect,
                                [this](const NetworkMessage& message) { HandlePeerDisconnected(message.senderID); });
        network.SetTimeoutHandler([this](Spark::Net::ClientID clientId) { HandlePeerDisconnected(clientId); });
        network.RegisterHandler(static_cast<MessageType>(FPSMessageType::PlayerInput),
                                [this](const NetworkMessage& message) { HandleInputMessage(message); });
        network.RegisterHandler(static_cast<MessageType>(FPSMessageType::StateSnapshot),
                                [this](const NetworkMessage& message) { HandleSnapshotMessage(message); });
    }

    void FPSMultiplayerSystem::UnregisterNetworkHandlers()
    {
        using Spark::Net::MessageType;
        auto& network = Spark::Net::NetworkManager::GetInstance();

        // Every observer above captures `this` and lives in this module image, and the
        // NetworkManager can outlive both (StopServer/Disconnect do not clear observers).
        // Remove them, and the timeout handler, when the session ends. NetworkManager
        // invokes copies, so this is safe from inside one of these callbacks.
        network.SetTimeoutHandler(nullptr);
        network.UnregisterHandler(MessageType::Connect);
        network.UnregisterHandler(MessageType::Disconnect);
        network.UnregisterHandler(static_cast<MessageType>(FPSMessageType::PlayerInput));
        network.UnregisterHandler(static_cast<MessageType>(FPSMessageType::StateSnapshot));
    }

    void FPSMultiplayerSystem::HandlePeerDisconnected(uint32_t clientId)
    {
        if (!m_isActive)
        {
            return;
        }

        if (m_isServer)
        {
            // The host's own player is never removed by a peer event.
            if (clientId != m_localClientId && m_playerStates.contains(clientId))
            {
                OnPlayerLeft(clientId);
            }
            return;
        }

        // A client hears Disconnect only from its server endpoint: the session is over.
        Spark::SimpleConsole::GetInstance().Log("[FPSMultiplayer] Server closed the session");
        Disconnect();
    }

    void FPSMultiplayerSystem::HandleInputMessage(const Spark::Net::NetworkMessage& message)
    {
        if (!m_isServer || !m_isActive)
        {
            return;
        }

        // senderID is stamped by NetworkManager from the admitted endpoint, never read off the wire.
        const uint32_t clientId = message.senderID;
        if (clientId == m_localClientId || !m_playerStates.contains(clientId))
        {
            return;
        }
        if (message.payload.size() != PlayerInput::SerializedSize)
        {
            return;
        }

        const auto budgetIt = m_inputBudget.find(clientId);
        if (budgetIt == m_inputBudget.end() || budgetIt->second + kInputBudgetEpsilon < kInputStep)
        {
            return;
        }

        // ApplyClientInput rejects non-finite, replayed and zero-sequence input; only an applied
        // input spends one step of the player's budget.
        const PlayerInput input = PlayerInput::Deserialize(message.payload.data(), message.payload.size());
        if (ApplyClientInput(clientId, input, kInputStep))
        {
            budgetIt->second = (std::max)(0.0f, budgetIt->second - kInputStep);
        }
    }

    void FPSMultiplayerSystem::HandleSnapshotMessage(const Spark::Net::NetworkMessage& message)
    {
        if (m_isServer || !m_isActive || m_localClientId == Spark::Net::INVALID_CLIENT)
        {
            return;
        }

        const std::vector<uint8_t>& payload = message.payload;
        if (payload.size() < kSnapshotHeaderSize)
        {
            return;
        }

        size_t offset = 0;
        const uint32_t batch = Detail::ReadU32(payload.data(), offset);
        const uint32_t count =
            static_cast<uint32_t>(payload[offset]) | (static_cast<uint32_t>(payload[offset + 1]) << 8);
        if (count > kMaxPlayers || payload.size() != kSnapshotHeaderSize + count * kSnapshotRecordSize)
        {
            return;
        }
        if (batch <= m_lastSnapshotBatch)
        {
            return;
        }

        auto recordAt = [&payload](uint32_t index)
        { return payload.data() + kSnapshotHeaderSize + static_cast<size_t>(index) * kSnapshotRecordSize; };

        // Validate the whole batch before applying any of it.
        for (uint32_t index = 0; index < count; ++index)
        {
            const NetworkPlayerState state =
                NetworkPlayerState::Deserialize(recordAt(index), NetworkPlayerState::SerializedSize);
            if (!IsFiniteState(state) || state.sequenceNumber != batch)
            {
                return;
            }
        }
        m_lastSnapshotBatch = batch;

        for (uint32_t index = 0; index < count; ++index)
        {
            const uint8_t* record = recordAt(index);
            const NetworkPlayerState state =
                NetworkPlayerState::Deserialize(record, NetworkPlayerState::SerializedSize);
            OnStateSnapshotReceived(state);

            size_t scoreOffset = NetworkPlayerState::SerializedSize;
            auto [scoreIt, inserted] = m_scores.try_emplace(state.clientId);
            PlayerScore& score = scoreIt->second;
            if (inserted)
            {
                score.playerName = "Player_" + std::to_string(state.clientId);
            }
            score.clientId = state.clientId;
            score.kills = Detail::ReadU32(record, scoreOffset);
            score.deaths = Detail::ReadU32(record, scoreOffset);
            score.assists = Detail::ReadU32(record, scoreOffset);
            score.score = static_cast<int32_t>(Detail::ReadU32(record, scoreOffset));
        }

        // The batch lists every player in the session; a remote player missing from it left.
        auto inBatch = [&](uint32_t clientId)
        {
            for (uint32_t index = 0; index < count; ++index)
            {
                size_t idOffset = 0;
                if (Detail::ReadU32(recordAt(index), idOffset) == clientId)
                {
                    return true;
                }
            }
            return false;
        };
        for (auto it = m_playerStates.begin(); it != m_playerStates.end();)
        {
            if (it->first != m_localClientId && !inBatch(it->first))
            {
                const uint32_t departed = it->first;
                ++it;
                OnPlayerLeft(departed);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = m_remoteSnapshots.begin(); it != m_remoteSnapshots.end();)
        {
            if (it->first != m_localClientId && !inBatch(it->first))
            {
                it = m_remoteSnapshots.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = m_scores.begin(); it != m_scores.end();)
        {
            if (it->first != m_localClientId && !inBatch(it->first))
            {
                it = m_scores.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // ============================================================================
    // Diagnostics
    // ============================================================================

    std::string FPSMultiplayerSystem::Console_GetStatus() const
    {
        std::string status = "FPSMultiplayer: ";
        if (!m_isActive)
        {
            status += "Inactive";
            return status;
        }

        if (m_isServer)
        {
            status += "Server";
        }
        else if (IsConnected())
        {
            status += "Client (connected, id " + std::to_string(m_localClientId) + ")";
        }
        else
        {
            status += "Client (connecting)";
        }
        status += " | Players: " + std::to_string(m_playerStates.size());
        status += " | Projectiles: " + std::to_string(m_projectiles.size());
        status += " | Tick: " + std::to_string(m_tickRate) + "Hz";
        status += " | Seq: " + std::to_string(m_stateSequence);
        auto metrics = GetDebugMetrics();
        status += " | RTT: " + std::to_string(static_cast<int>(metrics.rttMs)) + "ms";
        status += " | Loss: " + std::to_string(static_cast<int>(metrics.packetLossPercent)) + "%";
        status += " | Corrections: " + std::to_string(metrics.correctionCount);
        return status;
    }

    FPSMultiplayerSystem::MultiplayerDebugMetrics FPSMultiplayerSystem::GetDebugMetrics() const
    {
        MultiplayerDebugMetrics out;
        const auto& stats = Spark::Net::NetworkManager::GetInstance().GetStats();
        out.packetLossPercent = stats.packetLoss * 100.0f;
        out.rttMs = stats.ping;
        out.correctionCount = m_correctionCount;
        return out;
    }

} // namespace SparkFPS
