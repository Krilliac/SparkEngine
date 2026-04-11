/**
 * @file TestPersistentMaterialCB.cpp
 * @brief Phase P — CPU reference tests for PersistentMaterialCBManager
 *
 * Exercises the header-only
 * `Spark::Graphics::PersistentMaterialCBManager` class directly.
 * The manager is pure CPU — it tracks a shadow buffer + per-material
 * dirty bookkeeping without any GPU resources. Every test runs on
 * every platform's CI.
 *
 * Phase P also wires the manager as a per-instance member of
 * `Spark::MaterialSystem` with Initialize / Shutdown / BeginFrame
 * hooks on the shared implementation path (no Windows / Linux
 * divergence — `MaterialSystem::Initialize` runs the same code on
 * both branches apart from small logging differences).
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Graphics/PersistentMaterialCB.h"

#include <array>
#include <cstdint>
#include <cstring>

using Spark::Graphics::MaterialCBSlot;
using Spark::Graphics::PersistentMaterialCBManager;

namespace
{
    struct FakeMaterialData
    {
        float baseColor[4] = {1, 1, 1, 1};
        float metallic = 0.0f;
        float roughness = 0.5f;
        float emissive[3] = {0, 0, 0};
        float pad = 0;
    };
    static_assert(sizeof(FakeMaterialData) <= 256, "Fake material must fit in the CB slot");
} // namespace

// =========================================================================
// Lifecycle
// =========================================================================

TEST(PersistentMaterialCB_InitializeAndShutdown)
{
    PersistentMaterialCBManager mgr;
    EXPECT_FALSE(mgr.IsInitialized());
    mgr.Initialize(16, 256);
    EXPECT_TRUE(mgr.IsInitialized());
    EXPECT_EQ(mgr.GetRegisteredCount(), 0u);
    EXPECT_EQ(mgr.GetDirtyCount(), 0u);
    EXPECT_EQ(mgr.GetMegaBufferSize(), 16u * 256u);
    EXPECT_EQ(mgr.GetUsedSize(), 0u);
    mgr.Shutdown();
    EXPECT_FALSE(mgr.IsInitialized());
}

TEST(PersistentMaterialCB_DefaultInitializeValues)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(); // defaults: 4096 materials × 256 bytes = 1 MB
    EXPECT_TRUE(mgr.IsInitialized());
    EXPECT_EQ(mgr.GetMegaBufferSize(), 4096u * 256u);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_CBSizeAlignsTo16Bytes)
{
    // The cbSizePerMat argument is rounded up to a 16-byte multiple.
    PersistentMaterialCBManager mgr;
    mgr.Initialize(4, /*cbSizePerMat*/ 200); // 200 rounds up to 208 (next 16)
    // Expected total = 4 * 208 = 832.
    EXPECT_EQ(mgr.GetMegaBufferSize(), 4u * 208u);
    mgr.Shutdown();
}

// =========================================================================
// Registration
// =========================================================================

TEST(PersistentMaterialCB_RegisterBeforeInitializeFails)
{
    PersistentMaterialCBManager mgr;
    uint32_t offset = mgr.RegisterMaterial(1);
    EXPECT_EQ(offset, UINT32_MAX);
}

TEST(PersistentMaterialCB_RegisterMaterialAllocatesContiguousSlots)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    uint32_t a = mgr.RegisterMaterial(10);
    uint32_t b = mgr.RegisterMaterial(20);
    uint32_t c = mgr.RegisterMaterial(30);
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 256u);
    EXPECT_EQ(c, 512u);
    EXPECT_EQ(mgr.GetRegisteredCount(), 3u);
    EXPECT_EQ(mgr.GetUsedSize(), 768u);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_RegisterDuplicateReturnsExistingOffset)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    uint32_t first = mgr.RegisterMaterial(42);
    uint32_t second = mgr.RegisterMaterial(42);
    EXPECT_EQ(first, second);
    EXPECT_EQ(mgr.GetRegisteredCount(), 1u);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_RegisterMaterialOverflowReturnsInvalid)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(/*maxMaterials*/ 2, /*cbSizePerMat*/ 256);
    EXPECT_EQ(mgr.RegisterMaterial(1), 0u);
    EXPECT_EQ(mgr.RegisterMaterial(2), 256u);
    // Third registration exceeds the mega buffer size.
    EXPECT_EQ(mgr.RegisterMaterial(3), UINT32_MAX);
    // Registered count must still reflect only the successful ones.
    EXPECT_EQ(mgr.GetRegisteredCount(), 2u);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_GetBufferOffsetUnknownIsInvalid)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    EXPECT_EQ(mgr.GetBufferOffset(999), UINT32_MAX);
    mgr.RegisterMaterial(7);
    EXPECT_EQ(mgr.GetBufferOffset(7), 0u);
    EXPECT_EQ(mgr.GetBufferOffset(999), UINT32_MAX);
    mgr.Shutdown();
}

// =========================================================================
// Update + hash-based change detection
// =========================================================================

TEST(PersistentMaterialCB_UpdateUnknownMaterialFails)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    FakeMaterialData data;
    EXPECT_FALSE(mgr.UpdateMaterial(42, &data, sizeof(data))); // not registered
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_FirstUpdateMarksDirty)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    mgr.RegisterMaterial(1);
    FakeMaterialData data;
    data.metallic = 0.3f;
    EXPECT_TRUE(mgr.UpdateMaterial(1, &data, sizeof(data)));
    EXPECT_EQ(mgr.GetDirtyCount(), 1u);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_UpdateWithSameDataIsNoop)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    mgr.RegisterMaterial(1);

    FakeMaterialData data;
    data.metallic = 0.5f;
    EXPECT_TRUE(mgr.UpdateMaterial(1, &data, sizeof(data))); // first write

    // Clear dirty flags, then re-upload the same data — hash match → no-op.
    mgr.ClearDirtyFlags();
    EXPECT_FALSE(mgr.UpdateMaterial(1, &data, sizeof(data)));
    EXPECT_EQ(mgr.GetDirtyCount(), 0u);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_UpdateWithChangedDataMarksDirty)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    mgr.RegisterMaterial(1);

    FakeMaterialData data;
    data.metallic = 0.5f;
    mgr.UpdateMaterial(1, &data, sizeof(data));
    mgr.ClearDirtyFlags();

    // Mutate → the hash diverges → dirty.
    data.metallic = 0.75f;
    EXPECT_TRUE(mgr.UpdateMaterial(1, &data, sizeof(data)));
    EXPECT_EQ(mgr.GetDirtyCount(), 1u);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_UpdateSmallerThanSlotIsSafe)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    mgr.RegisterMaterial(1);

    // Upload only 32 bytes of data into a 256-byte slot — should succeed
    // and only mark the first 32 bytes as touched.
    std::array<uint8_t, 32> bytes{};
    for (int i = 0; i < 32; ++i)
        bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    EXPECT_TRUE(mgr.UpdateMaterial(1, bytes.data(), static_cast<uint32_t>(bytes.size())));
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_GetMaterialDataReturnsShadowPointer)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    mgr.RegisterMaterial(5);

    FakeMaterialData data;
    data.baseColor[0] = 0.25f;
    data.baseColor[1] = 0.5f;
    data.baseColor[2] = 0.75f;
    mgr.UpdateMaterial(5, &data, sizeof(data));

    const uint8_t* shadow = mgr.GetMaterialData(5);
    EXPECT_TRUE(shadow != nullptr);

    FakeMaterialData readBack;
    memcpy(&readBack, shadow, sizeof(readBack));
    EXPECT_NEAR(readBack.baseColor[0], 0.25f, 1e-6f);
    EXPECT_NEAR(readBack.baseColor[1], 0.5f, 1e-6f);
    EXPECT_NEAR(readBack.baseColor[2], 0.75f, 1e-6f);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_GetMaterialDataUnknownIsNull)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    EXPECT_TRUE(mgr.GetMaterialData(999) == nullptr);
    mgr.Shutdown();
}

// =========================================================================
// Dirty list + frame counter
// =========================================================================

TEST(PersistentMaterialCB_GetDirtySlotsReturnsOnlyDirty)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    mgr.RegisterMaterial(1);
    mgr.RegisterMaterial(2);
    mgr.RegisterMaterial(3);

    // New registrations mark slots as dirty (needsUpload = true).
    auto dirty = mgr.GetDirtySlots();
    EXPECT_EQ(static_cast<int>(dirty.size()), 3);

    mgr.ClearDirtyFlags();
    auto drained = mgr.GetDirtySlots();
    EXPECT_EQ(static_cast<int>(drained.size()), 0);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_ClearDirtyFlagsResetsDirtyCount)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    mgr.RegisterMaterial(1);
    FakeMaterialData data;
    mgr.UpdateMaterial(1, &data, sizeof(data));
    EXPECT_EQ(mgr.GetDirtyCount(), 1u);
    mgr.ClearDirtyFlags();
    EXPECT_EQ(mgr.GetDirtyCount(), 0u);
    mgr.Shutdown();
}

TEST(PersistentMaterialCB_BeginFrameAdvancesFrameStamp)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    mgr.RegisterMaterial(1);
    FakeMaterialData data;
    data.metallic = 0.25f;

    // Frame N: update metallic to 0.25
    mgr.BeginFrame();
    mgr.UpdateMaterial(1, &data, sizeof(data));
    auto dirty1 = mgr.GetDirtySlots();
    EXPECT_EQ(static_cast<int>(dirty1.size()), 1);
    uint32_t frame1 = dirty1[0]->lastUpdatedFrame;
    mgr.ClearDirtyFlags();

    // Frame N+1: change metallic so the hash diverges → new lastUpdatedFrame.
    mgr.BeginFrame();
    data.metallic = 0.5f;
    mgr.UpdateMaterial(1, &data, sizeof(data));
    auto dirty2 = mgr.GetDirtySlots();
    EXPECT_EQ(static_cast<int>(dirty2.size()), 1);
    EXPECT_GT(dirty2[0]->lastUpdatedFrame, frame1);
    mgr.Shutdown();
}

// =========================================================================
// Console status
// =========================================================================

TEST(PersistentMaterialCB_Console_GetStatusIncludesCounts)
{
    PersistentMaterialCBManager mgr;
    mgr.Initialize(16, 256);
    mgr.RegisterMaterial(1);
    mgr.RegisterMaterial(2);
    std::string status = mgr.Console_GetStatus();
    EXPECT_TRUE(status.find("PersistentMaterialCB") != std::string::npos);
    EXPECT_TRUE(status.find("2 materials") != std::string::npos);
    EXPECT_TRUE(status.find("dirty") != std::string::npos);
    mgr.Shutdown();
}
