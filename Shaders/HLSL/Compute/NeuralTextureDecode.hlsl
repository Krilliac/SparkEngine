/**
 * @file NeuralTextureDecode.hlsl
 * @brief Compute shader: decode a neural-compressed texture block to RGBA
 *
 * Each thread group decodes one block. Each thread within the group
 * evaluates the block's MLP for one pixel coordinate, producing an
 * RGBA output written to a UAV texture.
 */

#define MAX_NEURONS 256
#define MAX_LAYERS  8
#define MAX_FREQUENCIES 8
#define BLOCK_SIZE 16

cbuffer NTCDecodeCB : register(b0)
{
    uint g_BlocksX;            // Number of blocks horizontally
    uint g_BlocksY;            // Number of blocks vertically
    uint g_BlockSize;          // Pixels per block edge
    uint g_NumFrequencies;     // Positional encoding frequencies

    uint g_LayerCount;
    uint g_InputSize;          // MLP input size (with positional encoding)
    uint g_OutputSize;         // MLP output size (channels, typically 4)
    uint g_WeightsPerBlock;    // Total weights per block MLP

    uint g_LayerInputSizes[MAX_LAYERS];
    uint g_LayerOutputSizes[MAX_LAYERS];
    uint g_LayerActivations[MAX_LAYERS];
    uint g_LayerWeightOffsets[MAX_LAYERS];

    uint g_TextureWidth;
    uint g_TextureHeight;
    uint2 g_Padding;
};

StructuredBuffer<float> g_AllWeights : register(t0); // All block weights concatenated
RWTexture2D<float4>     g_OutputTex  : register(u0); // Decoded texture output

// Positional encoding: [u, v, sin(2^0 * pi * u), cos(2^0 * pi * u), ...]
void ComputePositionalEncoding(float u, float v, uint numFreq, out float encoded[MAX_NEURONS])
{
    uint idx = 0;
    encoded[idx++] = u;
    encoded[idx++] = v;

    float pi = 3.14159265358979f;
    for (uint f = 0; f < numFreq; ++f)
    {
        float scale = pow(2.0f, (float)f) * pi;
        encoded[idx++] = sin(scale * u);
        encoded[idx++] = cos(scale * u);
        encoded[idx++] = sin(scale * v);
        encoded[idx++] = cos(scale * v);
    }
}

float ApplyActivation(float x, uint activationType)
{
    switch (activationType)
    {
        case 0: return max(0.0f, x);          // ReLU
        case 1: return x > 0.0f ? x : 0.01f * x; // LeakyReLU
        case 2: return 1.0f / (1.0f + exp(-x));   // Sigmoid
        case 3: return tanh(x);                     // Tanh
        default: return x;                          // None
    }
}

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void CSNeuralTextureDecode(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    uint blockIdx = groupId.y * g_BlocksX + groupId.x;
    uint pixelX = groupId.x * g_BlockSize + groupThreadId.x;
    uint pixelY = groupId.y * g_BlockSize + groupThreadId.y;

    // Skip out-of-bounds pixels
    if (pixelX >= g_TextureWidth || pixelY >= g_TextureHeight)
        return;

    // Normalized UV within the block [0, 1]
    float u = ((float)groupThreadId.x + 0.5f) / (float)g_BlockSize;
    float v = ((float)groupThreadId.y + 0.5f) / (float)g_BlockSize;

    // Compute positional encoding
    float inputData[MAX_NEURONS];
    ComputePositionalEncoding(u, v, g_NumFrequencies, inputData);

    // Weight offset for this block
    uint blockWeightBase = blockIdx * g_WeightsPerBlock;

    // Forward pass through MLP
    float activationsA[MAX_NEURONS];
    float activationsB[MAX_NEURONS];

    // Copy input
    for (uint i = 0; i < g_InputSize; ++i)
        activationsA[i] = inputData[i];

    for (uint layer = 0; layer < g_LayerCount; ++layer)
    {
        uint inSz  = g_LayerInputSizes[layer];
        uint outSz = g_LayerOutputSizes[layer];
        uint act   = g_LayerActivations[layer];
        uint wOff  = blockWeightBase + g_LayerWeightOffsets[layer];
        bool readA = (layer % 2 == 0);

        for (uint j = 0; j < outSz; ++j)
        {
            float sum = 0.0f;
            uint rowOff = wOff + j * inSz;
            for (uint i = 0; i < inSz; ++i)
                sum += g_AllWeights[rowOff + i] * (readA ? activationsA[i] : activationsB[i]);
            sum += g_AllWeights[wOff + outSz * inSz + j];

            if (readA) activationsB[j] = ApplyActivation(sum, act);
            else       activationsA[j] = ApplyActivation(sum, act);
        }
    }

    // Read output from final buffer (odd layers → result in B, even → result in A)
    bool finalInB = (g_LayerCount % 2 != 0);
    float r = saturate(finalInB ? activationsB[0] : activationsA[0]);
    float g = saturate(finalInB ? activationsB[1] : activationsA[1]);
    float b = saturate(finalInB ? activationsB[2] : activationsA[2]);
    float a = g_OutputSize >= 4
                  ? saturate(finalInB ? activationsB[3] : activationsA[3])
                  : 1.0f;

    g_OutputTex[uint2(pixelX, pixelY)] = float4(r, g, b, a);
}
