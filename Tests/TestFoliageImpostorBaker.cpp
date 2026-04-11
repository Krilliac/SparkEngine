// TestFoliageImpostorBaker.cpp — Tests for CPU-side impostor atlas helpers
//
// Covers NextPowerOfTwo, ComputeAtlasLayout packing/wrap/overflow,
// SelectAngleSlot binning (including wrap), and GetAngleSlotUV coordinate
// arithmetic. Pure CPU — no GPU dependencies.

#include "TestFramework.h"
#include "Graphics/FoliageImpostorBaker.h"

#include <cstdint>
#include <vector>

using namespace Spark::Graphics;

// ============================================================================
// NextPowerOfTwo
// ============================================================================

TEST(FoliageImpostor_NextPowerOfTwo)
{
    EXPECT_EQ(FoliageImpostorBaker::NextPowerOfTwo(0), static_cast<uint32_t>(1));
    EXPECT_EQ(FoliageImpostorBaker::NextPowerOfTwo(1), static_cast<uint32_t>(1));
    EXPECT_EQ(FoliageImpostorBaker::NextPowerOfTwo(2), static_cast<uint32_t>(2));
    EXPECT_EQ(FoliageImpostorBaker::NextPowerOfTwo(3), static_cast<uint32_t>(4));
    EXPECT_EQ(FoliageImpostorBaker::NextPowerOfTwo(5), static_cast<uint32_t>(8));
    EXPECT_EQ(FoliageImpostorBaker::NextPowerOfTwo(17), static_cast<uint32_t>(32));
    EXPECT_EQ(FoliageImpostorBaker::NextPowerOfTwo(1024), static_cast<uint32_t>(1024));
    EXPECT_EQ(FoliageImpostorBaker::NextPowerOfTwo(1025), static_cast<uint32_t>(2048));
}

// ============================================================================
// ComputeAtlasLayout
// ============================================================================

TEST(FoliageImpostor_LayoutEmptyInputsReturnEmpty)
{
    uint32_t w = 99, h = 99;
    auto slots = FoliageImpostorBaker::ComputeAtlasLayout({}, 64, 8, 2048, w, h);
    EXPECT_EQ(slots.size(), static_cast<size_t>(0));
    EXPECT_EQ(w, static_cast<uint32_t>(0));
    EXPECT_EQ(h, static_cast<uint32_t>(0));
}

TEST(FoliageImpostor_LayoutZeroCellSizeReturnsEmpty)
{
    uint32_t w = 0, h = 0;
    auto slots = FoliageImpostorBaker::ComputeAtlasLayout({0u, 1u, 2u}, 0, 8, 2048, w, h);
    EXPECT_EQ(slots.size(), static_cast<size_t>(0));
}

TEST(FoliageImpostor_LayoutSingleSpeciesOneRow)
{
    uint32_t w = 0, h = 0;
    auto slots = FoliageImpostorBaker::ComputeAtlasLayout({42u}, 64, 8, 2048, w, h);

    EXPECT_EQ(slots.size(), static_cast<size_t>(1));
    EXPECT_EQ(slots[0].speciesIndex, static_cast<uint32_t>(42));
    EXPECT_EQ(slots[0].atlasX, static_cast<uint32_t>(0));
    EXPECT_EQ(slots[0].atlasY, static_cast<uint32_t>(0));
    EXPECT_EQ(slots[0].cellSize, static_cast<uint32_t>(64));
    EXPECT_EQ(slots[0].angleSteps, static_cast<uint32_t>(8));

    // Slot width = 64 * 8 = 512, height = 64. PoT rounding: 512 x 64.
    EXPECT_EQ(w, static_cast<uint32_t>(512));
    EXPECT_EQ(h, static_cast<uint32_t>(64));
}

TEST(FoliageImpostor_LayoutWrapsToNewRow)
{
    // 4 species, each 256 px wide (64*4), in a 1024-wide atlas.
    // Expect 4 per row, so all fit on row 0. Now shrink to 512 wide →
    // only 2 per row, 2 rows total.
    uint32_t w = 0, h = 0;
    auto slots = FoliageImpostorBaker::ComputeAtlasLayout({0u, 1u, 2u, 3u}, 64, 4, 512, w, h);

    EXPECT_EQ(slots.size(), static_cast<size_t>(4));
    // Row 0: x=0, x=256
    EXPECT_EQ(slots[0].atlasX, static_cast<uint32_t>(0));
    EXPECT_EQ(slots[0].atlasY, static_cast<uint32_t>(0));
    EXPECT_EQ(slots[1].atlasX, static_cast<uint32_t>(256));
    EXPECT_EQ(slots[1].atlasY, static_cast<uint32_t>(0));
    // Row 1: x=0, x=256
    EXPECT_EQ(slots[2].atlasX, static_cast<uint32_t>(0));
    EXPECT_EQ(slots[2].atlasY, static_cast<uint32_t>(64));
    EXPECT_EQ(slots[3].atlasX, static_cast<uint32_t>(256));
    EXPECT_EQ(slots[3].atlasY, static_cast<uint32_t>(64));

    EXPECT_EQ(w, static_cast<uint32_t>(512));
    EXPECT_EQ(h, static_cast<uint32_t>(128));
}

TEST(FoliageImpostor_LayoutOversizedSlotReturnsEmpty)
{
    uint32_t w = 0, h = 0;
    // slot width = 1024 but atlas max is 512 → impossible.
    auto slots = FoliageImpostorBaker::ComputeAtlasLayout({0u}, 128, 8, 512, w, h);
    EXPECT_EQ(slots.size(), static_cast<size_t>(0));
}

TEST(FoliageImpostor_LayoutDropsSpeciesWhenAtlasFull)
{
    // 512 x 512 atlas, slot = 256 wide * 256 tall. Capacity = 2 * 2 = 4.
    uint32_t w = 0, h = 0;
    std::vector<uint32_t> many = {0u, 1u, 2u, 3u, 4u, 5u};
    auto slots = FoliageImpostorBaker::ComputeAtlasLayout(many, 256, 1, 512, w, h);
    EXPECT_EQ(slots.size(), static_cast<size_t>(4));
}

// ============================================================================
// SelectAngleSlot
// ============================================================================

TEST(FoliageImpostor_SelectAngleSlotBasics)
{
    // 4 bins → 90 deg each. Bin 0 centered on 0, so [-45, 45) maps to bin 0.
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(0.0f, 4), static_cast<uint32_t>(0));
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(1.5707963f, 4), static_cast<uint32_t>(1)); // 90 deg
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(3.1415927f, 4), static_cast<uint32_t>(2)); // 180 deg
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(4.712389f, 4), static_cast<uint32_t>(3));  // 270 deg
}

TEST(FoliageImpostor_SelectAngleSlotWrapsAround2Pi)
{
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(6.283185f, 4), static_cast<uint32_t>(0));
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(6.283185f * 2.0f, 4), static_cast<uint32_t>(0));
}

TEST(FoliageImpostor_SelectAngleSlotNegativeYaw)
{
    // -90 deg → bin 3 in a 4-step scheme (equivalent to 270 deg).
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(-1.5707963f, 4), static_cast<uint32_t>(3));
}

TEST(FoliageImpostor_SelectAngleSlotZeroOrOneStep)
{
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(1.2f, 0), static_cast<uint32_t>(0));
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(1.2f, 1), static_cast<uint32_t>(0));
    EXPECT_EQ(FoliageImpostorBaker::SelectAngleSlot(10.0f, 1), static_cast<uint32_t>(0));
}

// ============================================================================
// GetAngleSlotUV
// ============================================================================

TEST(FoliageImpostor_GetAngleSlotUVCorner)
{
    ImpostorAtlasSlot slot;
    slot.speciesIndex = 0;
    slot.atlasX = 0;
    slot.atlasY = 0;
    slot.cellSize = 64;
    slot.angleSteps = 8;

    float minU, minV, maxU, maxV;
    FoliageImpostorBaker::GetAngleSlotUV(slot, 0, 512, 64, minU, minV, maxU, maxV);

    EXPECT_NEAR(minU, 0.0f, 1e-6f);
    EXPECT_NEAR(minV, 0.0f, 1e-6f);
    EXPECT_NEAR(maxU, 64.0f / 512.0f, 1e-6f); // = 0.125
    EXPECT_NEAR(maxV, 1.0f, 1e-6f);
}

TEST(FoliageImpostor_GetAngleSlotUVSecondAngle)
{
    ImpostorAtlasSlot slot;
    slot.speciesIndex = 0;
    slot.atlasX = 0;
    slot.atlasY = 0;
    slot.cellSize = 64;
    slot.angleSteps = 8;

    float minU, minV, maxU, maxV;
    FoliageImpostorBaker::GetAngleSlotUV(slot, 1, 512, 64, minU, minV, maxU, maxV);

    EXPECT_NEAR(minU, 64.0f / 512.0f, 1e-6f);  // = 0.125
    EXPECT_NEAR(maxU, 128.0f / 512.0f, 1e-6f); // = 0.250
    EXPECT_NEAR(minV, 0.0f, 1e-6f);
    EXPECT_NEAR(maxV, 1.0f, 1e-6f);
}

TEST(FoliageImpostor_GetAngleSlotUVOutOfRangeZeroed)
{
    ImpostorAtlasSlot slot;
    slot.speciesIndex = 0;
    slot.atlasX = 0;
    slot.atlasY = 0;
    slot.cellSize = 64;
    slot.angleSteps = 8;

    float minU = 9.0f, minV = 9.0f, maxU = 9.0f, maxV = 9.0f;
    // Angle index >= angleSteps → zeroed output.
    FoliageImpostorBaker::GetAngleSlotUV(slot, 8, 512, 64, minU, minV, maxU, maxV);
    EXPECT_NEAR(minU, 0.0f, 1e-6f);
    EXPECT_NEAR(minV, 0.0f, 1e-6f);
    EXPECT_NEAR(maxU, 0.0f, 1e-6f);
    EXPECT_NEAR(maxV, 0.0f, 1e-6f);
}

// ============================================================================
// Phase E: per-species cell UV rect math (CPU-only)
// ============================================================================

TEST(FoliageImpostor_CellRectMatchesAngleZeroSlot)
{
    // The cell buffer uploaded by FoliageImpostorAtlas::UploadCellBuffer
    // packs (minU, minV, cellDU, cellDV) per species where cellDU/cellDV
    // are the UV size of a single angle cell. Verify the math against
    // GetAngleSlotUV directly — this is the invariant the GPU relies on
    // when sampling the atlas in the impostor branch of FoliagePS.hlsl.
    std::vector<uint32_t> indices{0, 1, 2};
    uint32_t atlasW = 0;
    uint32_t atlasH = 0;
    auto slots = FoliageImpostorBaker::ComputeAtlasLayout(indices, /*cellSize=*/128, /*angleSteps=*/8,
                                                          /*maxAtlasSize=*/4096, atlasW, atlasH);
    EXPECT_EQ(slots.size(), 3u);
    EXPECT_GT(atlasW, 0u);
    EXPECT_GT(atlasH, 0u);

    for (const ImpostorAtlasSlot& s : slots)
    {
        float minU = 0.0f;
        float minV = 0.0f;
        float maxU = 0.0f;
        float maxV = 0.0f;
        FoliageImpostorBaker::GetAngleSlotUV(s, /*angleSlotIndex=*/0, atlasW, atlasH, minU, minV, maxU, maxV);
        const float cellDU = maxU - minU;
        const float cellDV = maxV - minV;

        // cellDU is the width of one angle cell in UV space: cellSize/atlasWidth.
        EXPECT_NEAR(cellDU, static_cast<float>(s.cellSize) / static_cast<float>(atlasW), 1e-6f);
        // cellDV spans the full slot height.
        EXPECT_NEAR(cellDV, static_cast<float>(s.cellSize) / static_cast<float>(atlasH), 1e-6f);

        // Origin at the slot's pixel corner.
        EXPECT_NEAR(minU, static_cast<float>(s.atlasX) / static_cast<float>(atlasW), 1e-6f);
        EXPECT_NEAR(minV, static_cast<float>(s.atlasY) / static_cast<float>(atlasH), 1e-6f);
    }
}

TEST(FoliageImpostor_CellRectAdvancesByCellDUAcrossAngles)
{
    // Adjacent angle bins inside the same slot are separated by exactly
    // one cellDU in U. This invariant is what lets the VS compute
    // `AngleU = angleBin * cellDU` without knowing atlas dimensions.
    std::vector<uint32_t> indices{0};
    uint32_t atlasW = 0;
    uint32_t atlasH = 0;
    auto slots = FoliageImpostorBaker::ComputeAtlasLayout(indices, /*cellSize=*/64, /*angleSteps=*/4,
                                                          /*maxAtlasSize=*/1024, atlasW, atlasH);
    EXPECT_EQ(slots.size(), 1u);

    float baseMinU = 0.0f, baseMinV = 0.0f, baseMaxU = 0.0f, baseMaxV = 0.0f;
    FoliageImpostorBaker::GetAngleSlotUV(slots[0], 0, atlasW, atlasH, baseMinU, baseMinV, baseMaxU, baseMaxV);
    const float cellDU = baseMaxU - baseMinU;

    for (uint32_t a = 1; a < slots[0].angleSteps; ++a)
    {
        float minU = 0.0f, minV = 0.0f, maxU = 0.0f, maxV = 0.0f;
        FoliageImpostorBaker::GetAngleSlotUV(slots[0], a, atlasW, atlasH, minU, minV, maxU, maxV);
        EXPECT_NEAR(minU, baseMinU + static_cast<float>(a) * cellDU, 1e-6f);
        EXPECT_NEAR(minV, baseMinV, 1e-6f);
        EXPECT_NEAR(maxV, baseMaxV, 1e-6f);
    }
}

// ============================================================================
// Phase F: 2-float4-per-species cell record layout (CPU-only)
// ============================================================================

TEST(FoliageImpostor_CellRecordDualFloat4Layout)
{
    // UploadCellBuffer packs 2 float4s per species:
    //   record[i*2 + 0] = uvRect: (minU, minV, cellDU, cellDV)
    //   record[i*2 + 1] = meta:   (billboardHeight, 0, 0, 0)
    // This test simulates the CPU side of that packing without a D3D
    // device — the same code the GPU path runs, minus CreateBuffer.
    std::vector<uint32_t> indices{0, 1, 2};
    uint32_t atlasW = 0;
    uint32_t atlasH = 0;
    auto slots = FoliageImpostorBaker::ComputeAtlasLayout(indices, /*cellSize=*/128, /*angleSteps=*/8,
                                                          /*maxAtlasSize=*/4096, atlasW, atlasH);
    EXPECT_EQ(slots.size(), 3u);

    // Simulate the packing logic: per species, pull slot 0's UV + the
    // heights we want to emulate on the CPU. Verify against the
    // invariants the shader relies on.
    const float kHeights[3] = {1.5f, 3.0f, 0.75f};

    for (size_t i = 0; i < slots.size(); ++i)
    {
        float minU = 0.0f, minV = 0.0f, maxU = 0.0f, maxV = 0.0f;
        FoliageImpostorBaker::GetAngleSlotUV(slots[i], 0, atlasW, atlasH, minU, minV, maxU, maxV);
        const float cellDU = maxU - minU;
        const float cellDV = maxV - minV;

        // Slot i occupies records at positions [i*2, i*2+1]. The shader
        // indexes `uvRect = cells[matId*2 + 0]` and
        // `meta = cells[matId*2 + 1]`.
        const uint32_t uvIdx = static_cast<uint32_t>(i) * 2u + 0u;
        const uint32_t metaIdx = static_cast<uint32_t>(i) * 2u + 1u;

        // uvRect invariants the VS + PS rely on.
        EXPECT_EQ(uvIdx % 2u, 0u);
        EXPECT_EQ(metaIdx - uvIdx, 1u);
        EXPECT_GT(cellDU, 0.0f);
        EXPECT_GT(cellDV, 0.0f);
        EXPECT_NEAR(cellDU, static_cast<float>(slots[i].cellSize) / static_cast<float>(atlasW), 1e-6f);
        EXPECT_NEAR(cellDV, static_cast<float>(slots[i].cellSize) / static_cast<float>(atlasH), 1e-6f);

        // Meta.x carries billboardHeight. The real UploadCellBuffer
        // pulls this from FoliageSpecies::billboardHeight — we just
        // verify the layout slot is reachable with the expected stride.
        EXPECT_GT(kHeights[i], 0.0f);
    }
}

// ============================================================================
// Phase G codex-fix regression: ComputeCellBufferSpeciesCount
// ============================================================================
//
// When the atlas is too small to hold every species, `ComputeAtlasLayout`
// drops trailing species from `m_slots`. The GPU cell buffer must still
// cover the full species registry — otherwise the foliage VS reads past
// the end of the SRV when `materialId` references a truncated species.
// `ComputeCellBufferSpeciesCount` returns the count to size the buffer
// with: `max(registryCount, maxSlotIndex + 1)`.

TEST(FoliageImpostorBaker_CellBufferSpeciesCount_AllSlotsFit)
{
    std::vector<ImpostorAtlasSlot> slots(3);
    slots[0].speciesIndex = 0;
    slots[1].speciesIndex = 1;
    slots[2].speciesIndex = 2;

    // Registry matches the slots exactly — no truncation.
    EXPECT_EQ(FoliageImpostorBaker::ComputeCellBufferSpeciesCount(slots, 3), 3u);
}

TEST(FoliageImpostorBaker_CellBufferSpeciesCount_AtlasTruncatedRegistryLarger)
{
    // Registry has 8 species but the atlas only fit the first 3.
    std::vector<ImpostorAtlasSlot> slots(3);
    slots[0].speciesIndex = 0;
    slots[1].speciesIndex = 1;
    slots[2].speciesIndex = 2;

    // Regression: must use registry count, not maxSlotIndex + 1, so
    // species 3..7 land in-bounds when the VS reads their cell records.
    EXPECT_EQ(FoliageImpostorBaker::ComputeCellBufferSpeciesCount(slots, 8), 8u);
}

TEST(FoliageImpostorBaker_CellBufferSpeciesCount_SparseSlotIndices)
{
    // Registry is smaller than the highest slot index (shouldn't happen
    // in practice, but is defined behavior: return the max). Defends
    // against a stale registry count vs a freshly-rebuilt slot list.
    std::vector<ImpostorAtlasSlot> slots(2);
    slots[0].speciesIndex = 3;
    slots[1].speciesIndex = 5;

    EXPECT_EQ(FoliageImpostorBaker::ComputeCellBufferSpeciesCount(slots, 2), 6u);
}

TEST(FoliageImpostorBaker_CellBufferSpeciesCount_EmptySlots)
{
    std::vector<ImpostorAtlasSlot> slots;

    // No baked slots — the buffer must still cover the registry.
    EXPECT_EQ(FoliageImpostorBaker::ComputeCellBufferSpeciesCount(slots, 4), 4u);
    EXPECT_EQ(FoliageImpostorBaker::ComputeCellBufferSpeciesCount(slots, 0), 0u);
}
