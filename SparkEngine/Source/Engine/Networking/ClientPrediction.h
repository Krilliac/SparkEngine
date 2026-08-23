/**
 * @file ClientPrediction.h
 * @brief Client-side prediction and server reconciliation for multiplayer FPS
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Implements client-side prediction with server reconciliation for responsive
 * multiplayer FPS gameplay. The client runs game logic locally for immediate
 * feedback, records inputs in a buffer, and reconciles with authoritative
 * server state when corrections arrive.
 *
 * ## Architecture
 * ```
 * ClientPrediction
 *   ├── m_inputBuffer      (circular buffer of recent inputs with sequence numbers)
 *   ├── m_pendingInputs    (inputs sent to server, awaiting ACK)
 *   ├── m_serverState      (most recent authoritative server state)
 *   └── Update()
 *         ├── RecordInput()       → stores input with sequence number
 *         ├── ApplyPrediction()   → runs input locally for instant response
 *         └── Reconcile()         → re-applies unACKed inputs after server correction
 * ```
 *
 * ## Usage
 * @code
 *   ClientPrediction prediction;
 *   prediction.SetMaxPendingInputs(128);
 *
 *   // Each tick:
 *   PredictedInput input;
 *   input.moveDir = GetInputDirection();
 *   input.jump = IsJumpPressed();
 *   prediction.RecordAndSend(input);
 *   prediction.ApplyPrediction(playerState, deltaTime);
 *
 *   // On server state received:
 *   prediction.OnServerState(serverState);
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <vector>
#include <cstdint>
#include <string>
#include <functional>

namespace Spark
{

    // =============================================================================
    // PredictedInput
    // =============================================================================

    /**
 * @brief A single input snapshot with sequence number for prediction.
 */
    struct PredictedInput
    {
        uint32_t sequenceNumber = 0;              ///< Monotonically increasing sequence
        float timestamp = 0.0f;                   ///< Client time when input was generated
        DirectX::XMFLOAT3 moveDirection{0, 0, 0}; ///< Normalized movement direction
        float lookYaw = 0.0f;                     ///< Camera yaw angle
        float lookPitch = 0.0f;                   ///< Camera pitch angle
        bool jump = false;                        ///< Jump pressed
        bool crouch = false;                      ///< Crouch pressed
        bool sprint = false;                      ///< Sprint pressed
        bool fire = false;                        ///< Fire pressed
        bool reload = false;                      ///< Reload pressed
        bool interact = false;                    ///< Interact pressed
        float simulationDeltaTime = 0.0f;         ///< Tick duration used for deterministic reconciliation replay
    };

    // =============================================================================
    // PredictedState
    // =============================================================================

    /**
 * @brief Predicted entity state resulting from input application.
 */
    struct PredictedState
    {
        uint32_t lastProcessedInput = 0; ///< Sequence number of last processed input
        DirectX::XMFLOAT3 position{0, 0, 0};
        DirectX::XMFLOAT3 velocity{0, 0, 0};
        float yaw = 0.0f;
        float pitch = 0.0f;
        bool isGrounded = true;
        bool isCrouching = false;
        bool isSprinting = false;
    };

    // =============================================================================
    // ClientPrediction
    // =============================================================================

    /**
 * @class ClientPrediction
 * @brief Client-side prediction with server reconciliation.
 */
    class ClientPrediction
    {
      public:
        ClientPrediction();
        ~ClientPrediction() = default;

        /** @brief Set maximum pending inputs before oldest are dropped. */
        void SetMaxPendingInputs(size_t max) { m_maxPendingInputs = max; }

        /**
     * @brief Record a new input and add to pending buffer.
     * @param input The input to record (sequence number is auto-assigned).
     * @return The assigned sequence number.
     */
        uint32_t RecordInput(const PredictedInput& input);

        /**
     * @brief Apply predicted movement locally.
     * @param state     Current predicted state (modified in-place).
     * @param input     Input to apply.
     * @param deltaTime Frame time.
     */
        void ApplyPrediction(PredictedState& state, const PredictedInput& input, float deltaTime);

        /**
     * @brief Receive authoritative server state and reconcile.
     *
     * Snaps to server state, then re-applies all inputs with sequence
     * numbers greater than the server's lastProcessedInput.
     *
     * @param serverState The authoritative state from the server.
     * @param deltaTime   Fixed tick time for re-simulation.
     */
        void Reconcile(const PredictedState& serverState, float deltaTime);

        /**
     * @brief Get the current reconciled/predicted state.
     */
        const PredictedState& GetState() const { return m_currentState; }

        /**
     * @brief Set the movement simulation function.
     *
     * This function applies one input to a state. The default built-in
     * handles basic FPS movement. Override for custom movement.
     *
     * @param func Function(state, input, deltaTime) that modifies state.
     */
        void SetMovementSimulator(std::function<void(PredictedState&, const PredictedInput&, float)> func);

        /** @brief Get the number of pending (unACKed) inputs. */
        size_t GetPendingInputCount() const { return m_pendingInputs.size(); }

        /** @brief Get the current sequence number. */
        uint32_t GetCurrentSequence() const { return m_sequenceNumber; }

        /** @brief Get the position correction magnitude from last reconciliation. */
        float GetLastCorrectionMagnitude() const { return m_lastCorrectionMag; }

        // --- Interpolation ---

        /**
     * @brief Enable smooth correction interpolation.
     * @param enabled Whether to interpolate corrections.
     * @param speed   Interpolation speed (higher = snappier).
     */
        void SetSmoothCorrection(bool enabled, float speed = 10.0f);

        // --- Console integration ---

        /** @brief Get prediction status (console integration). */
        std::string Console_GetStatus() const;

      private:
        void DefaultMovementSimulator(PredictedState& state, const PredictedInput& input, float deltaTime);

        std::vector<PredictedInput> m_pendingInputs;
        PredictedState m_currentState;
        uint32_t m_sequenceNumber = 0;
        uint32_t m_lastServerAck = 0;
        size_t m_maxPendingInputs = 128;
        float m_lastCorrectionMag = 0.0f;

        bool m_smoothCorrection = true;
        float m_correctionSpeed = 10.0f;
        DirectX::XMFLOAT3 m_correctionOffset{0, 0, 0};

        std::function<void(PredictedState&, const PredictedInput&, float)> m_movementSimulator;
    };

} // namespace Spark
