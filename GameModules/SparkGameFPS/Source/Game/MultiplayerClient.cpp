/**
 * @file MultiplayerClient.cpp
 * @brief FPSMultiplayerSystem client side: input prediction, reconciliation, and remote interpolation
 */

#include "MultiplayerSystem.h"

#include <algorithm>
#include <cmath>

namespace SparkFPS
{

    // ============================================================================
    // Client Input
    // ============================================================================

    void FPSMultiplayerSystem::SendInput(const PlayerInput& input)
    {
        if (!m_isActive)
            return;

        if (m_isServer)
        {
            // Listen server: the host's own input is authoritative and applied directly.
            const auto lastIt = m_lastInputByPlayer.find(m_localClientId);
            PlayerInput hostInput = input;
            hostInput.sequenceNumber = lastIt != m_lastInputByPlayer.end() ? lastIt->second.sequenceNumber + 1 : 1;
            ApplyClientInput(m_localClientId, hostInput, kInputStep);
            return;
        }

        // Until the handshake assigns an id there is no player to predict or to send for.
        if (m_localClientId == Spark::Net::INVALID_CLIENT)
            return;

        Spark::PredictedInput predicted{};
        predicted.timestamp = static_cast<float>(m_clientPrediction.GetCurrentSequence() + 1) * kInputStep;
        predicted.moveDirection = {input.strafe, 0.0f, input.forward};
        predicted.lookYaw = input.yaw;
        predicted.lookPitch = input.pitch;
        predicted.jump = input.jump;
        predicted.crouch = input.crouch;
        predicted.fire = input.fire;
        predicted.reload = input.reload;
        const uint32_t assignedSequence = m_clientPrediction.RecordInput(predicted);
        predicted.sequenceNumber = assignedSequence;

        PlayerInput sent = input;
        sent.sequenceNumber = assignedSequence;
        m_lastInputByPlayer[m_localClientId] = sent;

        m_clientPrediction.ApplyPrediction(m_localPredictedState, predicted, kInputStep);
        CopyPredictedMotionToLocal();

        // FPS input travels as its own message, not MessageType::ClientInput: NetworkManager
        // renumbers ClientInput sequences, and the server must acknowledge the sequence
        // client prediction assigned.
        Spark::Net::NetworkMessage message;
        message.type = static_cast<Spark::Net::MessageType>(FPSMessageType::PlayerInput);
        message.channel = Spark::Net::ChannelType::Unreliable;
        message.senderID = m_localClientId;
        message.payload = sent.Serialize();
        Spark::Net::NetworkManager::GetInstance().SendMessage(message);

        if (input.fire)
        {
            ProjectileData projectile;
            projectile.projectileId = m_nextProjectileId++;
            projectile.ownerId = m_localClientId;
            projectile.originX = m_localPredictedState.position.x;
            projectile.originY = m_localPredictedState.position.y + 1.0f;
            projectile.originZ = m_localPredictedState.position.z;
            projectile.positionX = projectile.originX;
            projectile.positionY = projectile.originY;
            projectile.positionZ = projectile.originZ;
            projectile.dirX = std::cos(input.yaw);
            projectile.dirY = 0.0f;
            projectile.dirZ = std::sin(input.yaw);
            projectile.velocityX = projectile.dirX * projectile.speed;
            projectile.velocityY = projectile.dirY * projectile.speed;
            projectile.velocityZ = projectile.dirZ * projectile.speed;
            projectile.active = true;
            m_projectiles[projectile.projectileId] = projectile; // local fire prediction hook
        }
    }
    // ============================================================================
    // Client Logic
    // ============================================================================

    void FPSMultiplayerSystem::ClientUpdate(float dt)
    {
        // Reconcile only against a server snapshot. Reconciling against the local
        // player's own predicted state would replay every pending input on top of
        // movement already applied, and the player would run away from the server.
        if (m_hasPendingLocalAuthority)
        {
            m_hasPendingLocalAuthority = false;
            ReconcileToAuthoritativeState(m_pendingLocalAuthority);
        }
        InterpolateRemotePlayers(dt);
        UpdateProjectiles(dt);
    }

    void FPSMultiplayerSystem::InterpolateRemotePlayers(float dt)
    {
        const float blend = (std::min)(1.0f, dt * 10.0f);
        for (auto& [playerId, snapshots] : m_remoteSnapshots)
        {
            if (playerId == m_localClientId || snapshots.empty())
                continue;

            auto target = snapshots.back();
            auto currentIt = m_playerStates.find(playerId);
            if (currentIt == m_playerStates.end())
            {
                m_playerStates[playerId] = target;
                continue;
            }

            auto& current = currentIt->second;
            current.posX += (target.posX - current.posX) * blend;
            current.posY += (target.posY - current.posY) * blend;
            current.posZ += (target.posZ - current.posZ) * blend;
            current.velX = target.velX;
            current.velY = target.velY;
            current.velZ = target.velZ;
            current.yaw = target.yaw;
            current.pitch = target.pitch;
            current.actionFlags = target.actionFlags;
            current.health = target.health;
            current.isAlive = target.isAlive;
            current.isCrouching = target.isCrouching;
            current.currentWeapon = target.currentWeapon;
            current.sequenceNumber = target.sequenceNumber;
        }
    }

    void FPSMultiplayerSystem::OnStateSnapshotReceived(const NetworkPlayerState& snapshot)
    {
        // Until the handshake assigns an id, m_localClientId is INVALID_CLIENT (0), which is
        // also the host's player id: the host's snapshots would be reconciled as our own.
        if (m_isServer || m_localClientId == Spark::Net::INVALID_CLIENT)
            return;

        if (snapshot.clientId == m_localClientId)
        {
            // Server snapshot sequences start at 1; 0 is a default-constructed or
            // truncated payload and never authoritative.
            if (snapshot.sequenceNumber <= m_lastLocalAuthoritySequence)
                return;
            m_lastLocalAuthoritySequence = snapshot.sequenceNumber;
            m_pendingLocalAuthority = snapshot;
            m_hasPendingLocalAuthority = true;
            return;
        }

        auto& history = m_remoteSnapshots[snapshot.clientId];
        if (!history.empty() && snapshot.sequenceNumber <= history.back().sequenceNumber)
            return;
        history.push_back(snapshot);
        while (history.size() > kMaxSnapshotHistory)
            history.pop_front();
    }

    void FPSMultiplayerSystem::ReconcileToAuthoritativeState(const NetworkPlayerState& authoritativeState)
    {
        Spark::PredictedState serverState;
        serverState.position = {authoritativeState.posX, authoritativeState.posY, authoritativeState.posZ};
        serverState.velocity = {authoritativeState.velX, authoritativeState.velY, authoritativeState.velZ};
        serverState.yaw = authoritativeState.yaw;
        serverState.pitch = authoritativeState.pitch;
        serverState.isCrouching = authoritativeState.isCrouching;
        serverState.lastProcessedInput = authoritativeState.acknowledgedInputSequence;

        const float before = m_clientPrediction.GetLastCorrectionMagnitude();
        m_clientPrediction.Reconcile(serverState, kInputStep);
        const float after = m_clientPrediction.GetLastCorrectionMagnitude();
        if (after > 0.01f && after != before)
        {
            ++m_correctionCount;
            Spark::Net::NetworkManager::GetInstance().SetPredictionCorrectionCount(m_correctionCount);
        }

        // Later inputs predict from the reconciled state, not the pre-snapshot one.
        m_localPredictedState = m_clientPrediction.GetState();

        // Health, life and score-relevant fields are server-owned; motion is the
        // reconciled prediction.
        auto& local = m_playerStates[m_localClientId];
        local.clientId = m_localClientId;
        local.health = authoritativeState.health;
        local.isAlive = authoritativeState.isAlive;
        local.currentWeapon = authoritativeState.currentWeapon;
        local.actionFlags = authoritativeState.actionFlags;
        local.acknowledgedInputSequence = authoritativeState.acknowledgedInputSequence;
        local.sequenceNumber = authoritativeState.sequenceNumber;
        CopyPredictedMotionToLocal();
    }

    void FPSMultiplayerSystem::CopyPredictedMotionToLocal()
    {
        auto localIt = m_playerStates.find(m_localClientId);
        if (localIt == m_playerStates.end())
            return;

        const auto& predicted = m_clientPrediction.GetState();
        auto& local = localIt->second;
        local.posX = predicted.position.x;
        local.posY = predicted.position.y;
        local.posZ = predicted.position.z;
        local.velX = predicted.velocity.x;
        local.velY = predicted.velocity.y;
        local.velZ = predicted.velocity.z;
        local.yaw = predicted.yaw;
        local.pitch = predicted.pitch;
        local.isCrouching = predicted.isCrouching;
    }

} // namespace SparkFPS
