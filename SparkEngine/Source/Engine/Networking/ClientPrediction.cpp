/**
 * @file ClientPrediction.cpp
 * @brief Implementation of client-side prediction and server reconciliation
 */

#include "ClientPrediction.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Spark
{

    ClientPrediction::ClientPrediction()
    {
        m_movementSimulator = [this](PredictedState& s, const PredictedInput& i, float dt)
        { DefaultMovementSimulator(s, i, dt); };
    }

    uint32_t ClientPrediction::RecordInput(const PredictedInput& input)
    {
        PredictedInput recorded = input;
        recorded.sequenceNumber = ++m_sequenceNumber;
        m_pendingInputs.push_back(recorded);

        // Trim oldest if over limit
        while (m_pendingInputs.size() > m_maxPendingInputs)
        {
            m_pendingInputs.erase(m_pendingInputs.begin());
        }

        return recorded.sequenceNumber;
    }

    void ClientPrediction::ApplyPrediction(PredictedState& state, const PredictedInput& input, float deltaTime)
    {
        PredictedInput appliedInput = input;
        if (appliedInput.sequenceNumber == 0 && !m_pendingInputs.empty())
            appliedInput.sequenceNumber = m_pendingInputs.back().sequenceNumber;

        const auto pending = std::find_if(m_pendingInputs.rbegin(), m_pendingInputs.rend(), [&](PredictedInput& value)
                                          { return value.sequenceNumber == appliedInput.sequenceNumber; });
        if (pending != m_pendingInputs.rend())
            pending->simulationDeltaTime = deltaTime;
        appliedInput.simulationDeltaTime = deltaTime;

        m_movementSimulator(state, appliedInput, deltaTime);
        state.lastProcessedInput = appliedInput.sequenceNumber;
        m_currentState = state;
    }

    void ClientPrediction::Reconcile(const PredictedState& serverState, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (serverState.lastProcessedInput < m_lastServerAck)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Ignoring stale reconciliation ACK %u (latest=%u)",
                            serverState.lastProcessedInput, m_lastServerAck);
            return;
        }
        m_lastServerAck = serverState.lastProcessedInput;
        const PredictedState previousState = m_currentState;

        // Snap to server state
        PredictedState reconciled = serverState;

        // Remove all inputs that the server has already processed
        m_pendingInputs.erase(std::remove_if(m_pendingInputs.begin(), m_pendingInputs.end(),
                                             [&](const PredictedInput& input)
                                             { return input.sequenceNumber <= serverState.lastProcessedInput; }),
                              m_pendingInputs.end());

        // Re-apply all remaining pending inputs
        for (const auto& input : m_pendingInputs)
        {
            const float replayDeltaTime = input.simulationDeltaTime > 0.0f ? input.simulationDeltaTime : deltaTime;
            m_movementSimulator(reconciled, input, replayDeltaTime);
            reconciled.lastProcessedInput = input.sequenceNumber;
        }

        // Report the correction actually applied after replaying unacknowledged
        // inputs. Comparing directly with the raw server snapshot incorrectly
        // counts legitimate pending movement as error.
        const float dx = reconciled.position.x - previousState.position.x;
        const float dy = reconciled.position.y - previousState.position.y;
        const float dz = reconciled.position.z - previousState.position.z;
        m_lastCorrectionMag = std::sqrt(dx * dx + dy * dy + dz * dz);
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Reconcile: correction magnitude=%.4f, pending inputs=%zu",
                        m_lastCorrectionMag, m_pendingInputs.size());

        if (m_smoothCorrection && m_lastCorrectionMag > 0.001f)
        {
            // Store correction offset for smooth interpolation
            m_correctionOffset.x = previousState.position.x - reconciled.position.x;
            m_correctionOffset.y = previousState.position.y - reconciled.position.y;
            m_correctionOffset.z = previousState.position.z - reconciled.position.z;
        }

        m_currentState = reconciled;
    }

    void ClientPrediction::SetMovementSimulator(std::function<void(PredictedState&, const PredictedInput&, float)> func)
    {
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, func != nullptr, "Movement simulator function must not be null");
        m_movementSimulator = std::move(func);
    }

    void ClientPrediction::SetSmoothCorrection(bool enabled, float speed)
    {
        m_smoothCorrection = enabled;
        m_correctionSpeed = speed;
    }

    void ClientPrediction::DefaultMovementSimulator(PredictedState& state, const PredictedInput& input, float deltaTime)
    {
        const float moveSpeed = input.sprint ? 8.0f : (input.crouch ? 2.5f : 5.0f);
        const float gravity = -9.81f;
        const float jumpForce = 5.0f;

        // Apply movement
        state.position.x += input.moveDirection.x * moveSpeed * deltaTime;
        state.position.y += input.moveDirection.y * moveSpeed * deltaTime;
        state.position.z += input.moveDirection.z * moveSpeed * deltaTime;

        // Apply gravity
        if (!state.isGrounded)
        {
            state.velocity.y += gravity * deltaTime;
            state.position.y += state.velocity.y * deltaTime;
        }

        // Simple ground check (before jump so jumping from ground works)
        if (state.position.y < 0.0f)
        {
            state.position.y = 0.0f;
            state.velocity.y = 0.0f;
            state.isGrounded = true;
        }

        // Jump
        if (input.jump && state.isGrounded)
        {
            state.velocity.y = jumpForce;
            state.isGrounded = false;
        }

        // Look direction
        state.yaw = input.lookYaw;
        state.pitch = input.lookPitch;
        state.isCrouching = input.crouch;
        state.isSprinting = input.sprint;
    }

    std::string ClientPrediction::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Client Prediction ===\n";
        oss << "Sequence: " << m_sequenceNumber << "\n";
        oss << "Pending inputs: " << m_pendingInputs.size() << "/" << m_maxPendingInputs << "\n";
        oss << "Last correction: " << m_lastCorrectionMag << " units\n";
        oss << "Smooth correction: " << (m_smoothCorrection ? "ON" : "OFF") << "\n";
        oss << "Position: (" << m_currentState.position.x << ", " << m_currentState.position.y << ", "
            << m_currentState.position.z << ")\n";
        oss << "Grounded: " << (m_currentState.isGrounded ? "YES" : "NO") << "\n";
        return oss.str();
    }

} // namespace Spark
