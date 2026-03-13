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
        m_movementSimulator(state, input, deltaTime);
        state.lastProcessedInput = input.sequenceNumber;
        m_currentState = state;
    }

    void ClientPrediction::Reconcile(const PredictedState& serverState, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        // Calculate correction magnitude
        float dx = serverState.position.x - m_currentState.position.x;
        float dy = serverState.position.y - m_currentState.position.y;
        float dz = serverState.position.z - m_currentState.position.z;
        m_lastCorrectionMag = std::sqrt(dx * dx + dy * dy + dz * dz);

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
            m_movementSimulator(reconciled, input, deltaTime);
        }

        if (m_smoothCorrection && m_lastCorrectionMag > 0.001f)
        {
            // Store correction offset for smooth interpolation
            m_correctionOffset.x = m_currentState.position.x - reconciled.position.x;
            m_correctionOffset.y = m_currentState.position.y - reconciled.position.y;
            m_correctionOffset.z = m_currentState.position.z - reconciled.position.z;
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

        // Jump
        if (input.jump && state.isGrounded)
        {
            state.velocity.y = jumpForce;
            state.isGrounded = false;
        }

        // Simple ground check
        if (state.position.y <= 0.0f)
        {
            state.position.y = 0.0f;
            state.velocity.y = 0.0f;
            state.isGrounded = true;
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
