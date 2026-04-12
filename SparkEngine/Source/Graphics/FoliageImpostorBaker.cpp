/**
 * @file FoliageImpostorBaker.cpp
 * @brief CPU-portable impostor atlas layout and UV math
 *
 * GPU-side atlas baking (FoliageImpostorAtlas) lives in
 * FoliageImpostorBakerGPU.cpp. This file contains only the portable
 * static helpers: atlas layout, angle slot selection, UV computation.
 *
 * Packing strategy: a simple row-major placement. Every species slot has
 * identical height (`cellSize`) and width (`cellSize * angleSteps`). We
 * advance horizontally until the next slot would exceed `maxAtlasSize`,
 * then wrap to a new row. This is not SAH-optimal but foliage atlases are
 * small (dozens of species, not thousands) and the uniform slot size
 * keeps sampling indices trivial to compute at draw time.
 */

#include "FoliageImpostorBaker.h"

// GPU-only includes live in FoliageImpostorBakerGPU.cpp

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Spark::Graphics
{

    namespace
    {
        constexpr float TWO_PI = 6.283185307179586f;
    }

    uint32_t FoliageImpostorBaker::NextPowerOfTwo(uint32_t v)
    {
        if (v <= 1)
            return 1;
        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        return v + 1;
    }

    std::vector<ImpostorAtlasSlot> FoliageImpostorBaker::ComputeAtlasLayout(const std::vector<uint32_t>& speciesIndices,
                                                                            uint32_t cellSize, uint32_t angleSteps,
                                                                            uint32_t maxAtlasSize,
                                                                            uint32_t& outAtlasWidth,
                                                                            uint32_t& outAtlasHeight)
    {
        outAtlasWidth = 0;
        outAtlasHeight = 0;

        if (cellSize == 0 || angleSteps == 0 || maxAtlasSize == 0 || speciesIndices.empty())
        {
            return {};
        }

        const uint32_t slotWidth = cellSize * angleSteps;
        if (slotWidth > maxAtlasSize)
        {
            // A single slot cannot fit even into a maxAtlasSize-wide atlas.
            return {};
        }

        std::vector<ImpostorAtlasSlot> slots;
        slots.reserve(speciesIndices.size());

        uint32_t cursorX = 0;
        uint32_t cursorY = 0;
        uint32_t usedWidth = 0;
        uint32_t usedHeight = cellSize;

        for (uint32_t species : speciesIndices)
        {
            if (cursorX + slotWidth > maxAtlasSize)
            {
                // Wrap to a new row.
                cursorX = 0;
                cursorY += cellSize;
            }

            if (cursorY + cellSize > maxAtlasSize)
            {
                // Out of atlas space — drop remaining species rather than
                // silently corrupting coordinates.
                break;
            }

            ImpostorAtlasSlot slot;
            slot.speciesIndex = species;
            slot.atlasX = cursorX;
            slot.atlasY = cursorY;
            slot.cellSize = cellSize;
            slot.angleSteps = angleSteps;
            slots.push_back(slot);

            cursorX += slotWidth;
            usedWidth = std::max(usedWidth, cursorX);
            usedHeight = std::max(usedHeight, cursorY + cellSize);
        }

        // Round used dimensions up to power of two for GPU-friendly sizes.
        outAtlasWidth = std::min(NextPowerOfTwo(usedWidth), maxAtlasSize);
        outAtlasHeight = std::min(NextPowerOfTwo(usedHeight), maxAtlasSize);

        return slots;
    }

    uint32_t FoliageImpostorBaker::ComputeCellBufferSpeciesCount(const std::vector<ImpostorAtlasSlot>& slots,
                                                                 uint32_t registrySpeciesCount)
    {
        uint32_t maxSlotIndex = 0;
        bool anySlot = false;
        for (const ImpostorAtlasSlot& s : slots)
        {
            maxSlotIndex = std::max(maxSlotIndex, s.speciesIndex);
            anySlot = true;
        }
        const uint32_t slotCeiling = anySlot ? (maxSlotIndex + 1) : 0;
        return std::max(registrySpeciesCount, slotCeiling);
    }

    uint32_t FoliageImpostorBaker::SelectAngleSlot(float yawRadians, uint32_t angleSteps)
    {
        if (angleSteps == 0)
            return 0;
        if (angleSteps == 1)
            return 0;

        // Wrap into [0, 2*pi).
        float wrapped = std::fmod(yawRadians, TWO_PI);
        if (wrapped < 0.0f)
            wrapped += TWO_PI;

        // Bin so that yaw=0 lands on the centre of bin 0.
        const float stepAngle = TWO_PI / static_cast<float>(angleSteps);
        const float shifted = wrapped + stepAngle * 0.5f;
        uint32_t bin = static_cast<uint32_t>(shifted / stepAngle);
        if (bin >= angleSteps)
            bin = 0; // wrap edge case when shifted >= 2*pi
        return bin;
    }

    void FoliageImpostorBaker::GetAngleSlotUV(const ImpostorAtlasSlot& slot, uint32_t angleSlotIndex,
                                              uint32_t atlasWidth, uint32_t atlasHeight, float& outMinU, float& outMinV,
                                              float& outMaxU, float& outMaxV)
    {
        outMinU = 0.0f;
        outMinV = 0.0f;
        outMaxU = 0.0f;
        outMaxV = 0.0f;

        if (atlasWidth == 0 || atlasHeight == 0 || slot.cellSize == 0 || slot.angleSteps == 0 ||
            angleSlotIndex >= slot.angleSteps)
        {
            return;
        }

        const uint32_t px = slot.atlasX + angleSlotIndex * slot.cellSize;
        const uint32_t py = slot.atlasY;

        const float invW = 1.0f / static_cast<float>(atlasWidth);
        const float invH = 1.0f / static_cast<float>(atlasHeight);

        outMinU = static_cast<float>(px) * invW;
        outMinV = static_cast<float>(py) * invH;
        outMaxU = static_cast<float>(px + slot.cellSize) * invW;
        outMaxV = static_cast<float>(py + slot.cellSize) * invH;
    }


    // FoliageImpostorAtlas GPU implementation lives in FoliageImpostorBakerGPU.cpp.

} // namespace Spark::Graphics
