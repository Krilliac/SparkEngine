/**
 * @file NeuralDenoise.hlsl
 * @brief Compute shader: neural patch-based denoiser
 *
 * Processes 8x8 pixel patches through a small MLP to denoise noisy
 * ray-traced or path-traced output. Each thread group handles one patch.
 */

#define MAX_NEURONS 256
#define MAX_LAYERS  8
#define PATCH_SIZE  8

cbuffer NeuralDenoiseCB : register(b0)
{
    uint g_ImageWidth;
    uint g_ImageHeight;
    uint g_PatchesX;        // ceil(width / PATCH_SIZE)
    uint g_PatchesY;        // ceil(height / PATCH_SIZE)

    uint g_UseAlbedoGuide;  // bool: include albedo in input
    uint g_UseNormalGuide;  // bool: include normal in input
    uint g_MLPLayerCount;
    uint g_MLPInputSize;

    uint g_MLPLayerInputSizes[MAX_LAYERS];
    uint g_MLPLayerOutputSizes[MAX_LAYERS];
    uint g_MLPLayerActivations[MAX_LAYERS];
    uint g_MLPLayerWeightOffsets[MAX_LAYERS];
};

Texture2D<float4> g_NoisyColor : register(t0);
Texture2D<float4> g_Albedo     : register(t1);
Texture2D<float4> g_Normal     : register(t2);
StructuredBuffer<float> g_MLPWeights : register(t3);

RWTexture2D<float4> g_DenoisedOutput : register(u0);

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

[numthreads(PATCH_SIZE, PATCH_SIZE, 1)]
void CSNeuralDenoise(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    uint px = groupId.x * PATCH_SIZE + threadId.x;
    uint py = groupId.y * PATCH_SIZE + threadId.y;

    if (px >= g_ImageWidth || py >= g_ImageHeight)
        return;

    // Read the noisy color at this pixel
    float3 noisyColor = g_NoisyColor[uint2(px, py)].rgb;

    // Build per-pixel MLP input: color + optional guides
    float mlpInput[MAX_NEURONS];
    uint inputIdx = 0;

    // Normalized position within patch
    mlpInput[inputIdx++] = float(threadId.x) / float(PATCH_SIZE);
    mlpInput[inputIdx++] = float(threadId.y) / float(PATCH_SIZE);

    // Noisy color
    mlpInput[inputIdx++] = noisyColor.x;
    mlpInput[inputIdx++] = noisyColor.y;
    mlpInput[inputIdx++] = noisyColor.z;

    // Optional albedo guide
    if (g_UseAlbedoGuide)
    {
        float3 albedo = g_Albedo[uint2(px, py)].rgb;
        mlpInput[inputIdx++] = albedo.x;
        mlpInput[inputIdx++] = albedo.y;
        mlpInput[inputIdx++] = albedo.z;
    }

    // Optional normal guide
    if (g_UseNormalGuide)
    {
        float3 normal = g_Normal[uint2(px, py)].rgb;
        mlpInput[inputIdx++] = normal.x;
        mlpInput[inputIdx++] = normal.y;
        mlpInput[inputIdx++] = normal.z;
    }

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

    // Output: denoised RGB (odd layers → result in B, even → result in A)
    bool finalInB = (g_MLPLayerCount % 2 != 0);
    float3 denoised;
    denoised.x = saturate(finalInB ? activationsB[0] : activationsA[0]);
    denoised.y = saturate(finalInB ? activationsB[1] : activationsA[1]);
    denoised.z = saturate(finalInB ? activationsB[2] : activationsA[2]);

    g_DenoisedOutput[uint2(px, py)] = float4(denoised, 1.0f);
}
