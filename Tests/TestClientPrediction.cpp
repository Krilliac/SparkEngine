/**
 * @file TestClientPrediction.cpp
 * @brief Tests for Spark::ClientPrediction
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Networking/ClientPrediction.h"

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
