/**
 * @file NeuralRadianceCacheQuery.hlsl
 * @brief Compute shader: query neural radiance cache for indirect lighting
 *
 * Looks up the multi-resolution hash grid at a world position, concatenates
 * features with view direction, and evaluates the MLP to produce RGB radiance.
 */

#define MAX_NEURONS 256
#define MAX_LAYERS  8
#define HASH_GRID_LEVELS 16
#define FEATURES_PER_ENTRY 2

cbuffer RadianceCacheQueryCB : register(b0)
{
    uint g_QueryCount;
    uint g_HashTableSize;
    uint g_MLPLayerCount;
    uint g_MLPInputSize;

    float g_MinResolution;
    float g_MaxResolution;
    uint2 g_Padding;

    uint g_MLPLayerInputSizes[MAX_LAYERS];
    uint g_MLPLayerOutputSizes[MAX_LAYERS];
    uint g_MLPLayerActivations[MAX_LAYERS];
    uint g_MLPLayerWeightOffsets[MAX_LAYERS];
};

struct QueryInput
{
    float3 position;
    float3 direction;
};

StructuredBuffer<QueryInput> g_Queries   : register(t0);
StructuredBuffer<float>      g_HashGrid  : register(t1);
StructuredBuffer<float>      g_MLPWeights : register(t2);

RWStructuredBuffer<float> g_Output : register(u0); // queryCount * 3 (RGB)

uint HashPosition(float3 pos, uint level, float resolution, uint tableSize)
{
    int3 cell = int3(floor(pos / resolution));
    uint hash = uint(cell.x) * 1u;
    hash ^= uint(cell.y) * 2654435761u;
    hash ^= uint(cell.z) * 805459861u;
    return hash % tableSize;
}

float GetLevelResolution(uint level)
{
    float t = float(level) / float(HASH_GRID_LEVELS - 1);
    return g_MinResolution * pow(g_MaxResolution / g_MinResolution, t);
}

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

[numthreads(64, 1, 1)]
void CSRadianceCacheQuery(uint3 dtid : SV_DispatchThreadID)
{
    uint queryIdx = dtid.x;
    if (queryIdx >= g_QueryCount)
        return;

    QueryInput query = g_Queries[queryIdx];

    // Multi-resolution hash grid feature lookup
    uint featureSize = HASH_GRID_LEVELS * FEATURES_PER_ENTRY;
    float mlpInput[MAX_NEURONS];

    for (uint level = 0; level < HASH_GRID_LEVELS; ++level)
    {
        float resolution = GetLevelResolution(level);
        uint idx = HashPosition(query.position, level, resolution, g_HashTableSize);
        uint gridBase = level * g_HashTableSize * FEATURES_PER_ENTRY + idx * FEATURES_PER_ENTRY;

        for (uint f = 0; f < FEATURES_PER_ENTRY; ++f)
            mlpInput[level * FEATURES_PER_ENTRY + f] = g_HashGrid[gridBase + f];
    }

    // Append direction
    mlpInput[featureSize + 0] = query.direction.x;
    mlpInput[featureSize + 1] = query.direction.y;
    mlpInput[featureSize + 2] = query.direction.z;

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

    // Write output (odd layers → result in B, even → result in A)
    bool finalInB = (g_MLPLayerCount % 2 != 0);
    uint outBase = queryIdx * 3;
    g_Output[outBase + 0] = finalInB ? activationsB[0] : activationsA[0];
    g_Output[outBase + 1] = finalInB ? activationsB[1] : activationsA[1];
    g_Output[outBase + 2] = finalInB ? activationsB[2] : activationsA[2];
}
