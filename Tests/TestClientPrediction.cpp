/**
 * @file TestClientPrediction.cpp
 * @brief Tests for Spark::ClientPrediction
 */

#include "TestFramework.h"
#include "Engine/Networking/ClientPrediction.h"

TEST(Prediction_RecordInput)
{
    Spark::ClientPrediction prediction;

    Spark::PredictedInput input;
    input.moveDirection = {1, 0, 0};
    input.jump = false;

    uint32_t seq1 = prediction.RecordInput(input);
    uint32_t seq2 = prediction.RecordInput(input);

    EXPECT_EQ(seq1, static_cast<uint32_t>(1));
    EXPECT_EQ(seq2, static_cast<uint32_t>(2));
    EXPECT_EQ(prediction.GetPendingInputCount(), static_cast<size_t>(2));
}

TEST(Prediction_ApplyMovement)
{
    Spark::ClientPrediction prediction;
    Spark::PredictedState state;
    state.position = {0, 0, 0};
    state.isGrounded = true;

    Spark::PredictedInput input;
    input.moveDirection = {1, 0, 0};
    input.sprint = false;

    prediction.ApplyPrediction(state, input, 1.0f / 60.0f);
    // Should have moved in X
    EXPECT_TRUE(state.position.x > 0.0f);
}

TEST(Prediction_Jump)
{
    Spark::ClientPrediction prediction;
    Spark::PredictedState state;
    state.position = {0, 0, 0};
    state.isGrounded = true;

    Spark::PredictedInput input;
    input.jump = true;

    prediction.ApplyPrediction(state, input, 1.0f / 60.0f);
    EXPECT_FALSE(state.isGrounded);
    EXPECT_TRUE(state.velocity.y > 0.0f);
}

TEST(Prediction_Reconcile)
{
    Spark::ClientPrediction prediction;

    // Record some inputs
    for (int i = 0; i < 5; ++i)
    {
        Spark::PredictedInput input;
        input.moveDirection = {1, 0, 0};
        prediction.RecordInput(input);
    }

    // Server corrects position
    Spark::PredictedState serverState;
    serverState.lastProcessedInput = 3; // Server processed up to input 3
    serverState.position = {10, 0, 0};

    prediction.Reconcile(serverState, 1.0f / 60.0f);

    // Pending inputs should only contain inputs 4 and 5
    EXPECT_EQ(prediction.GetPendingInputCount(), static_cast<size_t>(2));
}

TEST(Prediction_MaxPendingLimit)
{
    Spark::ClientPrediction prediction;
    prediction.SetMaxPendingInputs(10);

    for (int i = 0; i < 20; ++i)
    {
        Spark::PredictedInput input;
        prediction.RecordInput(input);
    }

    EXPECT_EQ(prediction.GetPendingInputCount(), static_cast<size_t>(10));
}

TEST(ClientPrediction_PredictAndReconcile)
{
    Spark::ClientPrediction prediction;

    // Predict 10 frames of forward movement
    Spark::PredictedState state;
    state.position = {0, 0, 0};
    state.isGrounded = true;

    for (int i = 0; i < 10; ++i)
    {
        Spark::PredictedInput input;
        input.moveDirection = {1, 0, 0};
        prediction.RecordInput(input);
        prediction.ApplyPrediction(state, input, 1.0f / 60.0f);
    }

    EXPECT_EQ(prediction.GetPendingInputCount(), static_cast<size_t>(10));
    EXPECT_TRUE(state.position.x > 0.0f);

    // Server corrects position at frame 5
    Spark::PredictedState serverState;
    serverState.lastProcessedInput = 5;
    serverState.position = {2.0f, 0, 0};
    serverState.isGrounded = true;

    prediction.Reconcile(serverState, 1.0f / 60.0f);

    // Only inputs 6-10 should remain pending
    EXPECT_EQ(prediction.GetPendingInputCount(), static_cast<size_t>(5));
}

TEST(ClientPrediction_NoCorrection_WhenAccurate)
{
    Spark::ClientPrediction prediction;

    Spark::PredictedState state;
    state.position = {0, 0, 0};
    state.isGrounded = true;

    // Record and apply one input
    Spark::PredictedInput input;
    input.moveDirection = {1, 0, 0};
    prediction.RecordInput(input);
    prediction.ApplyPrediction(state, input, 1.0f / 60.0f);

    // Server confirms exact same position
    Spark::PredictedState serverState;
    serverState.lastProcessedInput = 1;
    serverState.position = state.position;
    serverState.isGrounded = true;

    prediction.Reconcile(serverState, 1.0f / 60.0f);

    // Correction should be zero or near-zero
    EXPECT_NEAR(prediction.GetLastCorrectionMagnitude(), 0.0f, 0.01f);
}

TEST(ClientPrediction_SmoothCorrection)
{
    Spark::ClientPrediction prediction;
    prediction.SetSmoothCorrection(true, 10.0f);

    Spark::PredictedState state;
    state.position = {10, 0, 0};
    state.isGrounded = true;

    Spark::PredictedInput input;
    input.moveDirection = {0, 0, 0};
    prediction.RecordInput(input);
    prediction.ApplyPrediction(state, input, 1.0f / 60.0f);

    // Server says position is different
    Spark::PredictedState serverState;
    serverState.lastProcessedInput = 1;
    serverState.position = {5, 0, 0};
    serverState.isGrounded = true;

    prediction.Reconcile(serverState, 1.0f / 60.0f);

    // Correction magnitude should be non-zero (difference was 5 units)
    EXPECT_TRUE(prediction.GetLastCorrectionMagnitude() > 1.0f);
}

TEST(ClientPrediction_JitterResistance)
{
    Spark::ClientPrediction prediction;
    prediction.SetSmoothCorrection(true, 10.0f);

    Spark::PredictedState state;
    state.position = {0, 0, 0};
    state.isGrounded = true;

    // Simulate variable-latency inputs
    float prevX = 0.0f;
    for (int i = 0; i < 20; ++i)
    {
        Spark::PredictedInput input;
        input.moveDirection = {1, 0, 0};
        prediction.RecordInput(input);
        prediction.ApplyPrediction(state, input, 1.0f / 60.0f);

        // Simulate server confirmations with slight jitter
        if (i % 3 == 0 && i > 0)
        {
            Spark::PredictedState serverState;
            serverState.lastProcessedInput = static_cast<uint32_t>(i - 1);
            serverState.position = state.position;
            serverState.position.x += (i % 2 == 0 ? 0.001f : -0.001f); // Tiny jitter
            serverState.isGrounded = true;
            prediction.Reconcile(serverState, 1.0f / 60.0f);
        }

        // Position should always advance (no oscillation)
        EXPECT_TRUE(state.position.x >= prevX);
        prevX = state.position.x;
    }
}

TEST(ClientPrediction_InputReplay)
{
    Spark::ClientPrediction prediction;

    Spark::PredictedState state;
    state.position = {0, 0, 0};
    state.isGrounded = true;

    // Record 5 inputs with forward movement
    for (int i = 0; i < 5; ++i)
    {
        Spark::PredictedInput input;
        input.moveDirection = {1, 0, 0};
        prediction.RecordInput(input);
        prediction.ApplyPrediction(state, input, 1.0f / 60.0f);
    }

    // Server processed first 2, but from a different starting position
    Spark::PredictedState serverState;
    serverState.lastProcessedInput = 2;
    serverState.position = {0, 0, 0}; // Server says we're at origin
    serverState.isGrounded = true;

    prediction.Reconcile(serverState, 1.0f / 60.0f);

    // After reconciliation, inputs 3-5 should have been replayed
    EXPECT_EQ(prediction.GetPendingInputCount(), static_cast<size_t>(3));
    // Position should reflect 3 frames of forward movement from origin
    const auto& reconciledState = prediction.GetState();
    EXPECT_TRUE(reconciledState.position.x > 0.0f);
}

TEST(ClientPrediction_BufferOverflow)
{
    Spark::ClientPrediction prediction;
    prediction.SetMaxPendingInputs(5);

    // Record 10 inputs — should only keep last 5
    for (int i = 0; i < 10; ++i)
    {
        Spark::PredictedInput input;
        input.moveDirection = {1, 0, 0};
        prediction.RecordInput(input);
    }

    EXPECT_EQ(prediction.GetPendingInputCount(), static_cast<size_t>(5));
    EXPECT_EQ(prediction.GetCurrentSequence(), static_cast<uint32_t>(10));

    // Reconciliation with old sequence should still work gracefully
    Spark::PredictedState serverState;
    serverState.lastProcessedInput = 3; // Before buffer start
    serverState.position = {0, 0, 0};
    serverState.isGrounded = true;

    prediction.Reconcile(serverState, 1.0f / 60.0f);

    // All remaining inputs (6-10) should still be pending since they're > 3
    EXPECT_EQ(prediction.GetPendingInputCount(), static_cast<size_t>(5));
}
