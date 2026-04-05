/**
 * @file NeuralInference.hlsl
 * @brief Compute shader for batched MLP forward pass
 *
 * Evaluates a small multi-layer perceptron for a batch of input samples.
 * Each thread processes one sample through all layers sequentially.
 *
 * Activation types:
 *   0 = ReLU, 1 = LeakyReLU, 2 = Sigmoid, 3 = Tanh, 4 = None (linear)
 */

// Maximum neurons per layer (must match NeuralTypes.h kMaxNeuronsPerLayer)
#define MAX_NEURONS 256
#define MAX_LAYERS  8

// ---- Constant buffer (matches NeuralInferenceCB in NeuralTypes.h) ----
cbuffer NeuralCB : register(b0)
{
    uint g_LayerCount;
    uint g_BatchSize;
    uint g_InputSize;
    uint g_OutputSize;

    uint g_LayerInputSizes[MAX_LAYERS];
    uint g_LayerOutputSizes[MAX_LAYERS];
    uint g_LayerActivations[MAX_LAYERS];
    uint g_LayerWeightOffsets[MAX_LAYERS];
};

// ---- Resources ----
StructuredBuffer<float>   g_Weights : register(t0); // All weights + biases, concatenated
StructuredBuffer<float>   g_Input   : register(t1); // batchSize * inputSize
RWStructuredBuffer<float> g_Output  : register(u0); // batchSize * outputSize

// ---- Activation functions ----
float ApplyActivation(float x, uint activationType)
{
    switch (activationType)
    {
        case 0: // ReLU
            return max(0.0f, x);
        case 1: // LeakyReLU
            return x > 0.0f ? x : 0.01f * x;
        case 2: // Sigmoid
            return 1.0f / (1.0f + exp(-x));
        case 3: // Tanh
            return tanh(x);
        default: // None (linear)
            return x;
    }
}

// ---- Per-thread local storage for intermediate activations ----
// Two ping-pong buffers to hold current and previous layer activations
static float s_ActivationsA[MAX_NEURONS];
static float s_ActivationsB[MAX_NEURONS];

[numthreads(64, 1, 1)]
void CSNeuralForward(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint sampleIdx = dispatchThreadId.x;
    if (sampleIdx >= g_BatchSize)
        return;

    // Load input into activation buffer A
    uint inputOffset = sampleIdx * g_InputSize;
    for (uint i = 0; i < g_InputSize; ++i)
    {
        s_ActivationsA[i] = g_Input[inputOffset + i];
    }

    // Forward pass through each layer
    for (uint layer = 0; layer < g_LayerCount; ++layer)
    {
        uint inSize  = g_LayerInputSizes[layer];
        uint outSize = g_LayerOutputSizes[layer];
        uint activation = g_LayerActivations[layer];
        uint weightOffset = g_LayerWeightOffsets[layer];

        // Select source/destination buffers (ping-pong)
        // Even layers read from A, write to B; odd layers read from B, write to A
        bool readFromA = (layer % 2 == 0);

        for (uint j = 0; j < outSize; ++j)
        {
            // Matrix multiply: output[j] = dot(weights[j], input) + bias[j]
            float sum = 0.0f;

            // Weight matrix is stored row-major: weights[j * inSize + i]
            uint rowOffset = weightOffset + j * inSize;
            for (uint i = 0; i < inSize; ++i)
            {
                float inputVal = readFromA ? s_ActivationsA[i] : s_ActivationsB[i];
                sum += g_Weights[rowOffset + i] * inputVal;
            }

            // Add bias (biases stored after weight matrix)
            uint biasOffset = weightOffset + outSize * inSize + j;
            sum += g_Weights[biasOffset];

            // Apply activation
            float result = ApplyActivation(sum, activation);

            if (readFromA)
                s_ActivationsB[j] = result;
            else
                s_ActivationsA[j] = result;
        }
    }

    // Write final output
    // Layer 0 (even) writes to B, layer 1 (odd) writes to A, etc.
    // After N layers: result in B if N is odd, A if N is even.
    uint outputOffset = sampleIdx * g_OutputSize;
    bool finalInB = (g_LayerCount % 2 != 0);
    for (uint i = 0; i < g_OutputSize; ++i)
    {
        g_Output[outputOffset + i] = finalInB ? s_ActivationsB[i] : s_ActivationsA[i];
    }
}
