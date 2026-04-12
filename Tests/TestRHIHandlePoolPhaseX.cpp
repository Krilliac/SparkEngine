/**
 * @file TestRHIHandlePoolPhaseX.cpp
 * @brief Phase X real-class tests for Spark::RHI::HandlePool and TypedHandle
 *
 * Theme 3B phase 1. The existing Tests/TestRHIHandlePool.cpp exercises
 * a local reimplementation (`TestHandles::TypedHandle`, `HandlePool` in
 * an anonymous namespace) and never touches the real header at
 * `Graphics/RHI/RHIHandlePool.h`. The per-header @note explicitly says
 * the utility is "pure C++ with no GPU dependency; RHI device
 * implementations instantiate one per resource type as they are
 * added" — so the Theme 3B activation for this orphan is to lock down
 * the real class's public contract with portable tests.
 *
 * Coverage (against the real Spark::RHI::HandlePool<T, Tag> template):
 *
 *   - TypedHandle bit packing: Make(index, gen, tag) round-trips
 *     through GetIndex / GetGeneration / GetTag.
 *   - IsValid() is false for default-constructed handles, true for
 *     freshly allocated ones.
 *   - Allocate returns a valid handle on first call.
 *   - Get(handle) returns the resource pointer, nullptr on stale or
 *     invalid handles.
 *   - Free increments the generation counter — the old handle value
 *     becomes stale and Get returns nullptr.
 *   - Free on an invalid handle returns false.
 *   - Capacity is enforced (Allocate returns invalid when full).
 *   - Count tracks live entries; Clear resets all slots.
 *   - Reused slot after Free + Allocate has a bumped generation.
 *   - Tag discriminates at compile time: the BufferHandle alias has
 *     tag=1; TextureHandle has tag=2; etc.
 *   - Copied handles behave independently (value semantics).
 */

#include "TestFramework.h"
#include "Graphics/RHI/RHIHandlePool.h"

#include <cstdint>

namespace
{

    struct FakeBuffer
    {
        int id = 0;
    };

} // namespace

// ============================================================================
// TypedHandle bit packing
// ============================================================================

TEST(RHIHandlePoolPhaseX_TypedHandleRoundTrips)
{
    using H = Spark::RHI::TypedHandle<Spark::RHI::BufferTag>;
    auto h = H::Make(42, 7, 1);
    EXPECT_TRUE(h.IsValid());
    EXPECT_EQ(h.GetIndex(), static_cast<uint32_t>(42));
    EXPECT_EQ(h.GetGeneration(), static_cast<uint32_t>(7));
    EXPECT_EQ(h.GetTag(), static_cast<uint16_t>(1));
}

TEST(RHIHandlePoolPhaseX_DefaultHandleInvalid)
{
    Spark::RHI::BufferHandle h;
    EXPECT_FALSE(h.IsValid());
    EXPECT_EQ(h.GetIndex(), static_cast<uint32_t>(0));
    EXPECT_EQ(h.GetGeneration(), static_cast<uint32_t>(0));
    EXPECT_EQ(h.GetTag(), static_cast<uint16_t>(0));
}

TEST(RHIHandlePoolPhaseX_HandleEqualityByValue)
{
    auto a = Spark::RHI::BufferHandle::Make(1, 2, 1);
    auto b = Spark::RHI::BufferHandle::Make(1, 2, 1);
    auto c = Spark::RHI::BufferHandle::Make(1, 3, 1); // different generation
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
}

// ============================================================================
// Allocate / Get / Free lifecycle
// ============================================================================

TEST(RHIHandlePoolPhaseX_AllocateReturnsValidHandle)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 8> pool;
    FakeBuffer buf{.id = 99};
    auto h = pool.Allocate(&buf);
    EXPECT_TRUE(h.IsValid());
    EXPECT_EQ(pool.Count(), static_cast<uint32_t>(1));
}

TEST(RHIHandlePoolPhaseX_GetReturnsAllocatedPointer)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 8> pool;
    FakeBuffer buf{.id = 42};
    auto h = pool.Allocate(&buf);

    FakeBuffer* ptr = pool.Get(h);
    EXPECT_TRUE(ptr != nullptr);
    EXPECT_EQ(ptr->id, 42);
    EXPECT_TRUE(pool.IsValid(h));
}

TEST(RHIHandlePoolPhaseX_GetOnInvalidHandleReturnsNullptr)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 8> pool;
    Spark::RHI::BufferHandle invalid;
    EXPECT_TRUE(pool.Get(invalid) == nullptr);
    EXPECT_FALSE(pool.IsValid(invalid));
}

TEST(RHIHandlePoolPhaseX_FreeBumpsGenerationAndInvalidates)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 8> pool;
    FakeBuffer buf{.id = 1};
    auto h = pool.Allocate(&buf);
    const uint32_t genBefore = h.GetGeneration();

    EXPECT_TRUE(pool.Free(h));
    EXPECT_TRUE(pool.Get(h) == nullptr); // stale handle
    EXPECT_FALSE(pool.IsValid(h));
    EXPECT_EQ(pool.Count(), static_cast<uint32_t>(0));

    // Re-allocate into the same slot — generation must have advanced.
    auto h2 = pool.Allocate(&buf);
    EXPECT_TRUE(h2.IsValid());
    EXPECT_TRUE(h2.GetGeneration() > genBefore);
    // Old handle still stale even though index is reused.
    EXPECT_TRUE(pool.Get(h) == nullptr);
    EXPECT_TRUE(pool.Get(h2) != nullptr);
}

TEST(RHIHandlePoolPhaseX_FreeOnInvalidHandleReturnsFalse)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 8> pool;
    Spark::RHI::BufferHandle invalid;
    EXPECT_FALSE(pool.Free(invalid));
}

TEST(RHIHandlePoolPhaseX_DoubleFreeIsRejected)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 8> pool;
    FakeBuffer buf{.id = 1};
    auto h = pool.Allocate(&buf);

    EXPECT_TRUE(pool.Free(h));
    // Second Free on the same handle must report failure (stale).
    EXPECT_FALSE(pool.Free(h));
}

// ============================================================================
// Capacity enforcement
// ============================================================================

TEST(RHIHandlePoolPhaseX_CapacityEnforced)
{
    constexpr uint32_t kCap = 4;
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, kCap> pool;
    EXPECT_EQ(pool.GetCapacity(), kCap);

    FakeBuffer buf;
    std::vector<Spark::RHI::BufferHandle> handles;
    for (uint32_t i = 0; i < kCap; ++i)
    {
        auto h = pool.Allocate(&buf);
        EXPECT_TRUE(h.IsValid());
        handles.push_back(h);
    }
    EXPECT_EQ(pool.Count(), kCap);

    // Over-capacity allocation should return an invalid handle.
    auto overflow = pool.Allocate(&buf);
    EXPECT_FALSE(overflow.IsValid());
    EXPECT_EQ(pool.Count(), kCap);

    // Free one — the next Allocate should succeed again.
    EXPECT_TRUE(pool.Free(handles[0]));
    EXPECT_EQ(pool.Count(), kCap - 1);
    auto reused = pool.Allocate(&buf);
    EXPECT_TRUE(reused.IsValid());
    EXPECT_EQ(pool.Count(), kCap);
}

// ============================================================================
// Clear / Count / GetCapacity
// ============================================================================

TEST(RHIHandlePoolPhaseX_ClearResetsPool)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 16> pool;
    FakeBuffer buf;
    for (int i = 0; i < 5; ++i)
    {
        pool.Allocate(&buf);
    }
    EXPECT_EQ(pool.Count(), static_cast<uint32_t>(5));

    pool.Clear();
    EXPECT_EQ(pool.Count(), static_cast<uint32_t>(0));

    // After Clear, allocations are possible again up to capacity.
    auto h = pool.Allocate(&buf);
    EXPECT_TRUE(h.IsValid());
    EXPECT_EQ(pool.Count(), static_cast<uint32_t>(1));
}

// ============================================================================
// Tag discrimination — BufferTag vs TextureTag at compile time
// ============================================================================

TEST(RHIHandlePoolPhaseX_TagDiscriminatesHandleTypes)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 8> bufferPool;
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::TextureTag, 8> texturePool;

    FakeBuffer dummy;
    auto bufH = bufferPool.Allocate(&dummy);
    auto texH = texturePool.Allocate(&dummy);

    EXPECT_EQ(bufH.GetTag(), static_cast<uint16_t>(1));
    EXPECT_EQ(texH.GetTag(), static_cast<uint16_t>(2));
}

TEST(RHIHandlePoolPhaseX_AllTagsProduceDistinctValues)
{
    using namespace Spark::RHI;
    auto b = BufferHandle::Make(0, 1, 1);
    auto t = TextureHandle::Make(0, 1, 2);
    auto s = ShaderHandle::Make(0, 1, 3);
    auto sam = SamplerHandle::Make(0, 1, 4);
    auto p = PipelineHandle::Make(0, 1, 5);
    EXPECT_EQ(b.GetTag(), static_cast<uint16_t>(1));
    EXPECT_EQ(t.GetTag(), static_cast<uint16_t>(2));
    EXPECT_EQ(s.GetTag(), static_cast<uint16_t>(3));
    EXPECT_EQ(sam.GetTag(), static_cast<uint16_t>(4));
    EXPECT_EQ(p.GetTag(), static_cast<uint16_t>(5));
}

// ============================================================================
// Copied handle value semantics
// ============================================================================

TEST(RHIHandlePoolPhaseX_CopiedHandleStillValid)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 8> pool;
    FakeBuffer buf{.id = 7};
    auto h = pool.Allocate(&buf);
    auto copy = h;

    // The copy is still valid and resolves to the same pointer.
    FakeBuffer* ptrOriginal = pool.Get(h);
    FakeBuffer* ptrCopy = pool.Get(copy);
    EXPECT_TRUE(ptrOriginal != nullptr);
    EXPECT_TRUE(ptrCopy != nullptr);
    EXPECT_TRUE(ptrOriginal == ptrCopy);

    // Freeing the original invalidates both (they share the same
    // (index, generation) pair).
    pool.Free(h);
    EXPECT_TRUE(pool.Get(h) == nullptr);
    EXPECT_TRUE(pool.Get(copy) == nullptr);
}

// ============================================================================
// Multiple slot reuse pattern
// ============================================================================

TEST(RHIHandlePoolPhaseX_MultipleAllocFreeCyclesAdvanceGenerations)
{
    Spark::RHI::HandlePool<FakeBuffer, Spark::RHI::BufferTag, 4> pool;
    FakeBuffer buf;

    uint32_t lastGeneration = 0;
    for (int i = 0; i < 8; ++i)
    {
        auto h = pool.Allocate(&buf);
        EXPECT_TRUE(h.IsValid());
        // Freelist is LIFO-reversed; first slot reused → generation grows.
        if (i >= 1)
        {
            // After at least one free-realloc cycle the generation must
            // have grown from the initial (1). We cannot assert strict
            // monotonicity because multiple slots interleave, but we
            // verify at least one iteration produced a generation > 1.
            if (h.GetGeneration() > 1)
                lastGeneration = h.GetGeneration();
        }
        pool.Free(h);
    }

    EXPECT_TRUE(lastGeneration >= 2);
}
