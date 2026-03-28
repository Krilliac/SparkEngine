/**
 * @file HiZBuild.hlsl
 * @brief GPU compute shader for hierarchical Z-buffer mip generation
 *
 * Downsamples the depth buffer into a mip chain where each texel stores
 * the maximum depth of the 2x2 block from the previous level. Used by
 * GPUCull.hlsl for occlusion queries.
 *
 * Dispatch per mip: ceil(mipWidth / 16), ceil(mipHeight / 16), 1
 */

cbuffer HiZConstants : register(b0)
{
    uint2 inputSize;  // dimensions of the source mip level
    uint2 outputSize; // dimensions of the destination mip level
};

Texture2D<float>   inputMip : register(t0);
RWTexture2D<float> outputMip : register(u0);
SamplerState       pointSampler : register(s0);

groupshared float sharedDepths[16][16];

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID,
            uint3 groupThreadId : SV_GroupThreadID)
{
    uint2 texel = dispatchThreadId.xy;

    if (texel.x >= outputSize.x || texel.y >= outputSize.y)
        return;

    // Sample 2x2 block from the source mip
    uint2 srcBase = texel * 2;

    float d00 = inputMip[min(srcBase + uint2(0, 0), inputSize - 1)];
    float d10 = inputMip[min(srcBase + uint2(1, 0), inputSize - 1)];
    float d01 = inputMip[min(srcBase + uint2(0, 1), inputSize - 1)];
    float d11 = inputMip[min(srcBase + uint2(1, 1), inputSize - 1)];

    // Max of 2x2 block (conservative: farthest depth wins -> object must be
    // closer than all 4 texels to be occluded)
    float maxDepth = max(max(d00, d10), max(d01, d11));

    outputMip[texel] = maxDepth;
}
