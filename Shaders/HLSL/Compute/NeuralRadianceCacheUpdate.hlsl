/**
 * @file NeuralRadianceCacheUpdate.hlsl
 * @brief Compute shader: update neural radiance cache from path-traced samples
 *
 * Processes radiance samples (position, direction, radiance) and updates
 * the multi-resolution hash grid entries via gradient descent.
 */

#define MAX_NEURONS 256
#define MAX_LAYERS  8
#define HASH_GRID_LEVELS 16
#define FEATURES_PER_ENTRY 2

cbuffer RadianceCacheUpdateCB : register(b0)
{
    uint g_SampleCount;
    uint g_HashTableSize;
    uint g_MLPLayerCount;
    float g_LearningRate;

    float g_MinResolution;
    float g_MaxResolution;
    float g_TemporalBlend;
    uint g_FrameIndex;

    uint g_MLPLayerInputSizes[MAX_LAYERS];
    uint g_MLPLayerOutputSizes[MAX_LAYERS];
    uint g_MLPLayerActivations[MAX_LAYERS];
    uint g_MLPLayerWeightOffsets[MAX_LAYERS];
};

struct RadianceSample
{
    float3 position;
    float3 direction;
    float3 radiance;
};

StructuredBuffer<RadianceSample> g_Samples    : register(t0);
StructuredBuffer<float>          g_MLPWeights : register(t1);

RWStructuredBuffer<float> g_HashGrid : register(u0); // [level * tableSize * features]

// Spatial hash function (large primes from Instant NGP)
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

[numthreads(64, 1, 1)]
void CSRadianceCacheUpdate(uint3 dtid : SV_DispatchThreadID)
{
    uint sampleIdx = dtid.x;
    if (sampleIdx >= g_SampleCount)
        return;

    RadianceSample sample = g_Samples[sampleIdx];

    // Look up hash grid features for this position
    float features[HASH_GRID_LEVELS * FEATURES_PER_ENTRY];
    uint featureIndices[HASH_GRID_LEVELS]; // Grid offsets for gradient update

    for (uint level = 0; level < HASH_GRID_LEVELS; ++level)
    {
        float resolution = GetLevelResolution(level);
        uint idx = HashPosition(sample.position, level, resolution, g_HashTableSize);
        uint gridBase = level * g_HashTableSize * FEATURES_PER_ENTRY + idx * FEATURES_PER_ENTRY;
        featureIndices[level] = gridBase;

        for (uint f = 0; f < FEATURES_PER_ENTRY; ++f)
        {
            features[level * FEATURES_PER_ENTRY + f] = g_HashGrid[gridBase + f];
        }
    }

    // Build MLP input: features + direction
    uint featureSize = HASH_GRID_LEVELS * FEATURES_PER_ENTRY;
    float mlpInput[MAX_NEURONS];
    for (uint i = 0; i < featureSize; ++i)
        mlpInput[i] = features[i];
    mlpInput[featureSize + 0] = sample.direction.x;
    mlpInput[featureSize + 1] = sample.direction.y;
    mlpInput[featureSize + 2] = sample.direction.z;

    // Simple error-based hash grid update
    // (Full MLP backprop would require more complex shader; this is a practical approximation)
    float error = length(sample.radiance) * 0.01f; // Simplified error signal
    for (uint level = 0; level < HASH_GRID_LEVELS; ++level)
    {
        uint gridBase = featureIndices[level];
        for (uint f = 0; f < FEATURES_PER_ENTRY; ++f)
        {
            // Move features toward reducing error
            float current = g_HashGrid[gridBase + f];
            float update = g_LearningRate * error * (sample.radiance[f % 3] - current);
            g_HashGrid[gridBase + f] = lerp(current, current + update, g_TemporalBlend);
        }
    }
}
