/**
 * @file GPUOcclusionCulling.cpp
 * @brief CPU reference implementation of GPU-driven occlusion culling
 *
 * Builds a hierarchical Z-buffer (HiZ) pyramid from the previous frame's
 * depth buffer and tests object AABBs against it. The GPU version would
 * run as compute shader dispatches; this CPU implementation serves as
 * a reference and fallback.
 *
 * Pipeline: Depth Prepass -> Build HiZ -> Cull objects -> Draw visible
 */

#include "GPUOcclusionCulling.h"
#include "Utils/LogMacros.h"

#include <array>

namespace Spark::Graphics
{

    // =========================================================================
    // HiZPyramid implementation
    // =========================================================================

    void HiZPyramid::Build(const float* depthBuffer, uint32_t w, uint32_t h)
    {
        width = w;
        height = h;
        mipCount = 0;

        // Mip 0 = full resolution, each subsequent mip is half the previous
        uint32_t mw = w;
        uint32_t mh = h;
        while (mw >= 1 && mh >= 1 && mipCount < kMaxMips)
        {
            auto& mip = mips[mipCount];
            mip.width = mw;
            mip.height = mh;
            mip.depths.resize(mw * mh);

            if (mipCount == 0)
            {
                // Copy source depth buffer into mip 0
                for (uint32_t i = 0; i < mw * mh; ++i)
                {
                    mip.depths[i] = depthBuffer[i];
                }
            }
            else
            {
                // Downsample: take max of 2x2 block from previous mip
                // Max ensures conservative occlusion (farthest depth wins)
                const auto& prev = mips[mipCount - 1];
                for (uint32_t y = 0; y < mh; ++y)
                {
                    for (uint32_t x = 0; x < mw; ++x)
                    {
                        uint32_t px = std::min(x * 2, prev.width - 1);
                        uint32_t py = std::min(y * 2, prev.height - 1);
                        uint32_t px1 = std::min(px + 1, prev.width - 1);
                        uint32_t py1 = std::min(py + 1, prev.height - 1);

                        float d00 = prev.depths[py * prev.width + px];
                        float d10 = prev.depths[py * prev.width + px1];
                        float d01 = prev.depths[py1 * prev.width + px];
                        float d11 = prev.depths[py1 * prev.width + px1];

                        mip.depths[y * mw + x] = std::max({d00, d10, d01, d11});
                    }
                }
            }

            mipCount++;
            if (mw == 1 && mh == 1)
            {
                break;
            }
            mw = std::max(mw / 2, 1u);
            mh = std::max(mh / 2, 1u);
        }
    }

    float HiZPyramid::Sample(int mip, uint32_t x, uint32_t y) const
    {
        mip = std::clamp(mip, 0, mipCount - 1);
        const auto& m = mips[mip];
        x = std::min(x, m.width - 1);
        y = std::min(y, m.height - 1);
        return m.depths[y * m.width + x];
    }

    // =========================================================================
    // GPUOcclusionCuller implementation
    // =========================================================================

    void GPUOcclusionCuller::Initialize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GPUOcclusionCuller initialized: %ux%u", width, height);

        // GPU: create HiZ UAV texture, visibility buffer, indirect draw args
    }

    void GPUOcclusionCuller::Shutdown()
    {
        m_initialized = false;
    }

    void GPUOcclusionCuller::BuildHiZ(const float* depthBuffer, uint32_t w, uint32_t h)
    {
        m_hiZ.Build(depthBuffer, w, h);
        SPARK_LOG_TRACE(Spark::LogCategory::Graphics, "HiZ pyramid built: %ux%u, %d mip levels", w, h, m_hiZ.mipCount);

        // GPU: dispatch compute shader for each mip level
        // Each mip reads the previous mip and writes max of 2x2 blocks
    }

    bool GPUOcclusionCuller::IsVisible(float screenMinX, float screenMinY, float screenMaxX, float screenMaxY,
                                       float nearDepth) const
    {
        if (!m_initialized || m_hiZ.mipCount == 0)
        {
            return true; // Conservative: visible if no data
        }

        // Clamp screen-space bounds to [0,1]
        screenMinX = std::clamp(screenMinX, 0.0f, 1.0f);
        screenMinY = std::clamp(screenMinY, 0.0f, 1.0f);
        screenMaxX = std::clamp(screenMaxX, 0.0f, 1.0f);
        screenMaxY = std::clamp(screenMaxY, 0.0f, 1.0f);

        // Calculate screen extent in pixels at mip 0
        float extentX = (screenMaxX - screenMinX) * m_hiZ.width;
        float extentY = (screenMaxY - screenMinY) * m_hiZ.height;
        float maxExtent = std::max(extentX, extentY);

        // Select mip level where the AABB covers roughly 1-2 texels
        // This makes the test O(1) instead of scanning many pixels
        int mip = 0;
        if (maxExtent > 0)
        {
            mip = static_cast<int>(std::ceil(std::log2(maxExtent)));
        }
        mip = std::clamp(mip, 0, m_hiZ.mipCount - 1);

        // Sample HiZ at the four corners of the projected AABB
        const auto& mipLevel = m_hiZ.mips[mip];
        uint32_t x0 = static_cast<uint32_t>(screenMinX * mipLevel.width);
        uint32_t y0 = static_cast<uint32_t>(screenMinY * mipLevel.height);
        uint32_t x1 = static_cast<uint32_t>(screenMaxX * mipLevel.width);
        uint32_t y1 = static_cast<uint32_t>(screenMaxY * mipLevel.height);

        // Take the maximum depth from all four corners (conservative)
        float maxHiZ = 0.0f;
        maxHiZ = std::max(maxHiZ, m_hiZ.Sample(mip, x0, y0));
        maxHiZ = std::max(maxHiZ, m_hiZ.Sample(mip, x1, y0));
        maxHiZ = std::max(maxHiZ, m_hiZ.Sample(mip, x0, y1));
        maxHiZ = std::max(maxHiZ, m_hiZ.Sample(mip, x1, y1));

        // Object is occluded if its nearest depth is behind the farthest HiZ depth
        return nearDepth <= maxHiZ;
    }

    std::vector<uint32_t> GPUOcclusionCuller::CullBatch(const std::vector<OcclusionAABB>& objects,
                                                        const float* viewProjMatrix) const
    {
        std::vector<uint32_t> visible;
        visible.reserve(objects.size());

        if (!m_initialized || m_hiZ.mipCount == 0 || viewProjMatrix == nullptr)
        {
            // Conservative fallback: mark everything visible
            for (uint32_t i = 0; i < static_cast<uint32_t>(objects.size()); ++i)
            {
                visible.push_back(i);
            }
            return visible;
        }

        for (uint32_t i = 0; i < static_cast<uint32_t>(objects.size()); ++i)
        {
            const auto& aabb = objects[i];

            // Project all 8 AABB corners through the view-projection matrix
            // to find the screen-space bounding rectangle and nearest depth
            std::array<float, 3> corners[8] = {
                {aabb.minX, aabb.minY, aabb.minZ}, {aabb.maxX, aabb.minY, aabb.minZ}, {aabb.minX, aabb.maxY, aabb.minZ},
                {aabb.maxX, aabb.maxY, aabb.minZ}, {aabb.minX, aabb.minY, aabb.maxZ}, {aabb.maxX, aabb.minY, aabb.maxZ},
                {aabb.minX, aabb.maxY, aabb.maxZ}, {aabb.maxX, aabb.maxY, aabb.maxZ},
            };

            float screenMinX = 1.0f;
            float screenMinY = 1.0f;
            float screenMaxX = 0.0f;
            float screenMaxY = 0.0f;
            float nearestDepth = 1.0f;
            bool behindCamera = false;

            for (const auto& corner : corners)
            {
                // Multiply by 4x4 view-projection matrix (row-major)
                const float* m = viewProjMatrix;
                float cx = corner[0] * m[0] + corner[1] * m[4] + corner[2] * m[8] + m[12];
                float cy = corner[0] * m[1] + corner[1] * m[5] + corner[2] * m[9] + m[13];
                float cz = corner[0] * m[2] + corner[1] * m[6] + corner[2] * m[10] + m[14];
                float cw = corner[0] * m[3] + corner[1] * m[7] + corner[2] * m[11] + m[15];

                if (cw <= 0.0f)
                {
                    // Behind the camera -- mark visible conservatively
                    behindCamera = true;
                    break;
                }

                // Perspective divide to NDC [-1,1], then to screen UV [0,1]
                float invW = 1.0f / cw;
                float ndcX = cx * invW;
                float ndcY = cy * invW;
                float ndcZ = cz * invW;

                float sx = ndcX * 0.5f + 0.5f;
                float sy = ndcY * 0.5f + 0.5f;
                float depth = ndcZ * 0.5f + 0.5f;

                screenMinX = std::min(screenMinX, sx);
                screenMinY = std::min(screenMinY, sy);
                screenMaxX = std::max(screenMaxX, sx);
                screenMaxY = std::max(screenMaxY, sy);
                nearestDepth = std::min(nearestDepth, depth);
            }

            if (behindCamera || IsVisible(screenMinX, screenMinY, screenMaxX, screenMaxY, nearestDepth))
            {
                visible.push_back(i);
            }
        }

        SPARK_LOG_TRACE(Spark::LogCategory::Graphics, "OcclusionCull: %zu/%zu objects visible", visible.size(),
                        objects.size());
        return visible;
    }

    std::string GPUOcclusionCuller::Console_GetStatus() const
    {
        return "GPUOcclusionCuller: HiZ " + std::to_string(m_hiZ.width) + "x" + std::to_string(m_hiZ.height) + " (" +
               std::to_string(m_hiZ.mipCount) + " mips)\n";
    }

    // =========================================================================
    // GPU Resource Management
    // =========================================================================

    bool GPUOcclusionCuller::CreateGPUResources(Spark::RHI::IRHIDevice* device, uint32_t width, uint32_t height)
    {
        if (!device)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "GPUOcclusionCuller::CreateGPUResources: null device");
            return false;
        }

        if (width == 0 || height == 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "GPUOcclusionCuller::CreateGPUResources: invalid dimensions %ux%u", width, height);
            return false;
        }

        // Compute mip level count for the HiZ pyramid
        uint32_t maxDim = std::max(width, height);
        uint32_t mipLevels = 1;
        uint32_t temp = maxDim;
        while (temp > 1)
        {
            temp >>= 1;
            ++mipLevels;
        }
        mipLevels = std::min(mipLevels, static_cast<uint32_t>(HiZPyramid::kMaxMips));

        // Create HiZ mip chain texture (R32_FLOAT with mip levels)
        Spark::RHI::RHITextureDesc hiZDesc;
        hiZDesc.width = width;
        hiZDesc.height = height;
        hiZDesc.depth = 1;
        hiZDesc.mipLevels = mipLevels;
        hiZDesc.format = Spark::RHI::PixelFormat::R32_FLOAT;
        hiZDesc.type = Spark::RHI::RHITextureType::Texture2D;
        hiZDesc.usage = Spark::RHI::RHITextureUsage::ShaderResource;
        hiZDesc.debugName = "HiZ_MipChain";

        m_gpuHiZTexture = device->CreateTexture(hiZDesc);
        if (!m_gpuHiZTexture)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "GPUOcclusionCuller::CreateGPUResources: failed to create HiZ texture");
            return false;
        }

        // Create visibility result buffer
        // One uint32_t per potential object (use a reasonable default capacity)
        static constexpr uint32_t kMaxObjects = 65536;
        Spark::RHI::RHIBufferDesc visDesc;
        visDesc.size = static_cast<uint64_t>(kMaxObjects) * sizeof(uint32_t);
        visDesc.stride = sizeof(uint32_t);
        visDesc.usage = Spark::RHI::RHIBufferUsage::Structured;
        visDesc.access = Spark::RHI::RHIBufferAccess::Dynamic;
        visDesc.initialData = nullptr;
        visDesc.debugName = "HiZ_VisibilityBuffer";

        m_gpuVisibilityBuffer = device->CreateBuffer(visDesc);
        if (!m_gpuVisibilityBuffer)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "GPUOcclusionCuller::CreateGPUResources: failed to create visibility buffer");
            m_gpuHiZTexture.reset();
            return false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "GPUOcclusionCuller: GPU resources created (HiZ %ux%u R32F, %u mips, visibility buffer)", width,
                       height, mipLevels);
        return true;
    }

    void GPUOcclusionCuller::ResizeGPUResources(Spark::RHI::IRHIDevice* device, uint32_t width, uint32_t height)
    {
        if (!device)
        {
            return;
        }

        // Release existing resources and recreate at new dimensions
        m_gpuHiZTexture.reset();
        m_gpuVisibilityBuffer.reset();

        if (width > 0 && height > 0)
        {
            CreateGPUResources(device, width, height);
        }
    }

} // namespace Spark::Graphics
