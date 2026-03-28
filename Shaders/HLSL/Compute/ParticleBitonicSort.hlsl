/**
 * @file ParticleBitonicSort.hlsl
 * @brief GPU bitonic merge sort for particle depth ordering
 *
 * Sorts alive particle indices by camera distance for correct
 * alpha-blended transparency rendering (back-to-front).
 *
 * Two-pass approach:
 * - Pass 1 (CSBitonicSort): Local sort within each thread group (shared memory)
 * - Pass 2 (CSBitonicMerge): Global merge passes across groups
 */

cbuffer SortConstants : register(b0)
{
    uint level;         // current bitonic level
    uint descendMask;   // current sub-level mask
    uint aliveCount;    // total alive particles
    uint pad0;
};

// Sort keys (camera distance, negated for back-to-front)
RWStructuredBuffer<float> sortKeys : register(u0);

// Particle indices (reordered alongside keys)
RWStructuredBuffer<uint> aliveList : register(u1);

groupshared float sharedKeys[1024];
groupshared uint  sharedVals[1024];

// ---- Pass 1: Local bitonic sort within a thread group (up to 1024 elements) ----

[numthreads(512, 1, 1)]
void CSBitonicSort(uint3 groupId : SV_GroupID,
                   uint3 threadId : SV_GroupThreadID,
                   uint3 dispatchId : SV_DispatchThreadID)
{
    uint localIdx = threadId.x;
    uint globalBase = groupId.x * 1024;

    // Load two elements per thread into shared memory
    uint idx0 = globalBase + localIdx * 2;
    uint idx1 = idx0 + 1;

    sharedKeys[localIdx * 2]     = (idx0 < aliveCount) ? sortKeys[idx0] : 3.402823466e+38;
    sharedKeys[localIdx * 2 + 1] = (idx1 < aliveCount) ? sortKeys[idx1] : 3.402823466e+38;
    sharedVals[localIdx * 2]     = (idx0 < aliveCount) ? aliveList[idx0] : 0;
    sharedVals[localIdx * 2 + 1] = (idx1 < aliveCount) ? aliveList[idx1] : 0;

    GroupMemoryBarrierWithGroupSync();

    // Bitonic sort in shared memory
    for (uint k = 2; k <= 1024; k <<= 1)
    {
        for (uint j = k >> 1; j > 0; j >>= 1)
        {
            uint swapIdx = localIdx ^ j;
            if (swapIdx > localIdx)
            {
                // Determine sort direction
                bool ascending = ((localIdx & k) == 0);

                if ((sharedKeys[localIdx] > sharedKeys[swapIdx]) == ascending)
                {
                    // Swap keys
                    float tmpKey = sharedKeys[localIdx];
                    sharedKeys[localIdx] = sharedKeys[swapIdx];
                    sharedKeys[swapIdx] = tmpKey;

                    // Swap indices
                    uint tmpVal = sharedVals[localIdx];
                    sharedVals[localIdx] = sharedVals[swapIdx];
                    sharedVals[swapIdx] = tmpVal;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    // Write back to global memory
    if (idx0 < aliveCount)
    {
        sortKeys[idx0]  = sharedKeys[localIdx * 2];
        aliveList[idx0] = sharedVals[localIdx * 2];
    }
    if (idx1 < aliveCount)
    {
        sortKeys[idx1]  = sharedKeys[localIdx * 2 + 1];
        aliveList[idx1] = sharedVals[localIdx * 2 + 1];
    }
}

// ---- Pass 2: Global bitonic merge across groups ----

[numthreads(256, 1, 1)]
void CSBitonicMerge(uint3 dispatchId : SV_DispatchThreadID)
{
    uint index = dispatchId.x;
    if (index >= aliveCount)
        return;

    uint swapIdx = index ^ descendMask;
    if (swapIdx <= index || swapIdx >= aliveCount)
        return;

    // Determine sort direction based on current level
    bool ascending = ((index & level) == 0);

    float keyA = sortKeys[index];
    float keyB = sortKeys[swapIdx];

    if ((keyA > keyB) == ascending)
    {
        // Swap keys
        sortKeys[index]   = keyB;
        sortKeys[swapIdx] = keyA;

        // Swap indices
        uint valA = aliveList[index];
        uint valB = aliveList[swapIdx];
        aliveList[index]   = valB;
        aliveList[swapIdx] = valA;
    }
}
