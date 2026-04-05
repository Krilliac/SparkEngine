/**
 * @file NeuralSuperResolution.hlsl
 * @brief Compute shader: neural 2x super-resolution
 *
 * Upscales 8x8 low-res patches to 16x16 high-res patches using a small MLP.
 * Each thread handles one output pixel (16x16 thread group per patch).
 */

#define MAX_NEURONS 256
#define MAX_LAYERS  8
#define INPUT_PATCH  8
#define OUTPUT_PATCH 16

cbuffer NeuralSRCB : register(b0)
{
    uint g_InputWidth;       // Low-res input width
    uint g_InputHeight;      // Low-res input height
    uint g_OutputWidth;      // High-res output width (2x)
    uint g_OutputHeight;     // High-res output height (2x)

    uint g_PatchesX;
    uint g_PatchesY;
    uint g_UseDepthInput;    // bool
    uint g_MLPLayerCount;

    uint g_MLPInputSize;
    uint3 g_Padding;

    uint g_MLPLayerInputSizes[MAX_LAYERS];
    uint g_MLPLayerOutputSizes[MAX_LAYERS];
    uint g_MLPLayerActivations[MAX_LAYERS];
    uint g_MLPLayerWeightOffsets[MAX_LAYERS];
};

Texture2D<float4>       g_LowResColor : register(t0);
Texture2D<float>        g_Depth       : register(t1);
StructuredBuffer<float> g_MLPWeights  : register(t2);
SamplerState            g_LinearSampler : register(s0);

RWTexture2D<float4> g_HighResOutput : register(u0);

float ApplyActivation(float x, uint activationType)
{
    switch (activationType)
    {
        case 0: return max(0.0f, x);
        case 1: return x > 0.0f ? x : 0.01f * x;
        case 2: return 1.0f / (1.0f + exp(-x));
        case 3: return tanh(x);
        default: return x;
    }
}

[numthreads(OUTPUT_PATCH, OUTPUT_PATCH, 1)]
void CSNeuralSuperResolution(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    // Output pixel coordinate
    uint outX = groupId.x * OUTPUT_PATCH + threadId.x;
    uint outY = groupId.y * OUTPUT_PATCH + threadId.y;

    if (outX >= g_OutputWidth || outY >= g_OutputHeight)
        return;

    // Corresponding low-res coordinate (with 0.5 offset for pixel centers)
    float lowResU = (float(outX) + 0.5f) / float(g_OutputWidth);
    float lowResV = (float(outY) + 0.5f) / float(g_OutputHeight);

    // Sample low-res color (bilinear)
    float3 lowResColor = g_LowResColor.SampleLevel(g_LinearSampler, float2(lowResU, lowResV), 0).rgb;

    // Build MLP input
    float mlpInput[MAX_NEURONS];
    uint inputIdx = 0;

    // Sub-pixel position within the output patch [0, 1]
    mlpInput[inputIdx++] = float(threadId.x) / float(OUTPUT_PATCH);
    mlpInput[inputIdx++] = float(threadId.y) / float(OUTPUT_PATCH);

    // Low-res color at this position
    mlpInput[inputIdx++] = lowResColor.x;
    mlpInput[inputIdx++] = lowResColor.y;
    mlpInput[inputIdx++] = lowResColor.z;

    // Optional depth
    if (g_UseDepthInput)
    {
        float depth = g_Depth.SampleLevel(g_LinearSampler, float2(lowResU, lowResV), 0);
        mlpInput[inputIdx++] = depth;
    }

    // Also sample neighboring low-res pixels for context
    float2 texelSize = float2(1.0f / g_InputWidth, 1.0f / g_InputHeight);
    float3 colorLeft  = g_LowResColor.SampleLevel(g_LinearSampler, float2(lowResU - texelSize.x, lowResV), 0).rgb;
    float3 colorRight = g_LowResColor.SampleLevel(g_LinearSampler, float2(lowResU + texelSize.x, lowResV), 0).rgb;
    float3 colorUp    = g_LowResColor.SampleLevel(g_LinearSampler, float2(lowResU, lowResV - texelSize.y), 0).rgb;
    float3 colorDown  = g_LowResColor.SampleLevel(g_LinearSampler, float2(lowResU, lowResV + texelSize.y), 0).rgb;

    mlpInput[inputIdx++] = colorLeft.x;  mlpInput[inputIdx++] = colorLeft.y;  mlpInput[inputIdx++] = colorLeft.z;
    mlpInput[inputIdx++] = colorRight.x; mlpInput[inputIdx++] = colorRight.y; mlpInput[inputIdx++] = colorRight.z;
    mlpInput[inputIdx++] = colorUp.x;    mlpInput[inputIdx++] = colorUp.y;    mlpInput[inputIdx++] = colorUp.z;
    mlpInput[inputIdx++] = colorDown.x;  mlpInput[inputIdx++] = colorDown.y;  mlpInput[inputIdx++] = colorDown.z;

    // MLP forward pass
    float activationsA[MAX_NEURONS];
    float activationsB[MAX_NEURONS];

    for (uint i = 0; i < g_MLPInputSize; ++i)
        activationsA[i] = mlpInput[i];

    for (uint layer = 0; layer < g_MLPLayerCount; ++layer)
    {
        uint inSz  = g_MLPLayerInputSizes[layer];
        uint outSz = g_MLPLayerOutputSizes[layer];
        uint act   = g_MLPLayerActivations[layer];
        uint wOff  = g_MLPLayerWeightOffsets[layer];
        bool readA = (layer % 2 == 0);

        for (uint j = 0; j < outSz; ++j)
        {
            float sum = 0.0f;
            uint rowOff = wOff + j * inSz;
            for (uint i = 0; i < inSz; ++i)
                sum += g_MLPWeights[rowOff + i] * (readA ? activationsA[i] : activationsB[i]);
            sum += g_MLPWeights[wOff + outSz * inSz + j];

            if (readA) activationsB[j] = ApplyActivation(sum, act);
            else       activationsA[j] = ApplyActivation(sum, act);
        }
    }

    // Output: high-res RGB (odd layers → result in B, even → result in A)
    bool finalInB = (g_MLPLayerCount % 2 != 0);
    float3 highRes;
    highRes.x = saturate(finalInB ? activationsB[0] : activationsA[0]);
    highRes.y = saturate(finalInB ? activationsB[1] : activationsA[1]);
    highRes.z = saturate(finalInB ? activationsB[2] : activationsA[2]);

    g_HighResOutput[uint2(outX, outY)] = float4(highRes, 1.0f);
}
