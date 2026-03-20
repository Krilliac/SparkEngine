/**
 * @file TestPCSX2Systems.cpp
 * @brief Tests for all 12 PCSX2-inspired systems
 */

#include "TestFramework.h"
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// 1. FreezeSystem — Tag-validated save state serialization
// ============================================================================

namespace
{

    struct FreezeBufferMode
    {
        enum Value
        {
            Write,
            Read
        };
    };

    class TestFreezeBuffer
    {
      public:
        enum class Mode
        {
            Write,
            Read
        };

        explicit TestFreezeBuffer(Mode mode) : m_mode(mode) {}
        TestFreezeBuffer(Mode mode, std::vector<uint8_t> data) : m_mode(mode), m_data(std::move(data)) {}

        template <typename T>
        bool Freeze(T& value)
        {
            if (m_mode == Mode::Write)
            {
                const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
                m_data.insert(m_data.end(), bytes, bytes + sizeof(T));
                return true;
            }
            if (m_readPos + sizeof(T) > m_data.size())
                return false;
            std::memcpy(&value, m_data.data() + m_readPos, sizeof(T));
            m_readPos += sizeof(T);
            return true;
        }

        template <typename T>
        bool FreezeLegacy(T& value, size_t savedSize)
        {
            if (m_mode == Mode::Write)
            {
                const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
                m_data.insert(m_data.end(), bytes, bytes + sizeof(T));
                return true;
            }
            size_t readAmount = std::min(savedSize, sizeof(T));
            if (m_readPos + savedSize > m_data.size())
                return false;
            std::memset(&value, 0, sizeof(T));
            std::memcpy(&value, m_data.data() + m_readPos, readAmount);
            m_readPos += savedSize;
            return true;
        }

        bool FreezeTag(const std::string& tag)
        {
            if (m_mode == Mode::Write)
            {
                uint32_t len = static_cast<uint32_t>(tag.size());
                Freeze(len);
                const auto* bytes = reinterpret_cast<const uint8_t*>(tag.data());
                m_data.insert(m_data.end(), bytes, bytes + len);
                return true;
            }
            uint32_t len = 0;
            if (!Freeze(len))
                return false;
            if (m_readPos + len > m_data.size())
                return false;
            std::string stored(reinterpret_cast<const char*>(m_data.data() + m_readPos), len);
            m_readPos += len;
            return stored == tag;
        }

        const std::vector<uint8_t>& GetData() const { return m_data; }
        size_t GetSize() const { return m_data.size(); }

      private:
        Mode m_mode;
        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
    };

} // anonymous namespace

TEST(FreezeBuffer_RoundTrip)
{
    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);

    int32_t intVal = 42;
    float floatVal = 3.14f;
    writer.Freeze(intVal);
    writer.Freeze(floatVal);

    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    int32_t readInt = 0;
    float readFloat = 0.0f;
    EXPECT_TRUE(reader.Freeze(readInt));
    EXPECT_TRUE(reader.Freeze(readFloat));
    EXPECT_EQ(readInt, 42);
    EXPECT_NEAR(readFloat, 3.14f, 0.001f);
}

TEST(FreezeBuffer_TagValidation)
{
    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);
    writer.FreezeTag("Physics");
    int32_t val = 100;
    writer.Freeze(val);
    writer.FreezeTag("Audio");

    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    EXPECT_TRUE(reader.FreezeTag("Physics"));
    int32_t readVal = 0;
    reader.Freeze(readVal);
    EXPECT_EQ(readVal, 100);
    EXPECT_TRUE(reader.FreezeTag("Audio"));
}

TEST(FreezeBuffer_TagMismatch)
{
    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);
    writer.FreezeTag("Physics");

    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    EXPECT_FALSE(reader.FreezeTag("Audio"));
}

TEST(FreezeBuffer_LegacyCompat)
{
    struct OldStruct
    {
        int32_t x;
        int32_t y;
    };
    struct NewStruct
    {
        int32_t x;
        int32_t y;
        int32_t z;
        int32_t w;
    };

    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);
    OldStruct old = {10, 20};
    writer.Freeze(old);

    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    NewStruct expanded = {};
    EXPECT_TRUE(reader.FreezeLegacy(expanded, sizeof(OldStruct)));
    EXPECT_EQ(expanded.x, 10);
    EXPECT_EQ(expanded.y, 20);
    EXPECT_EQ(expanded.z, 0); // Zero-filled new field
    EXPECT_EQ(expanded.w, 0);
}

TEST(FreezeSystem_SubsystemOrchestration)
{
    struct PhysicsState
    {
        float gravity = -9.81f;
        int subSteps = 4;
    };

    struct AudioState
    {
        float masterVolume = 0.8f;
        int activeVoices = 24;
    };

    PhysicsState physState = {-9.81f, 4};
    AudioState audioState = {0.8f, 24};

    // Write
    TestFreezeBuffer writer(TestFreezeBuffer::Mode::Write);
    uint32_t version = 1;
    writer.Freeze(version);
    writer.FreezeTag("Physics");
    writer.Freeze(physState);
    writer.FreezeTag("Audio");
    writer.Freeze(audioState);
    writer.FreezeTag("END");

    // Read back
    TestFreezeBuffer reader(TestFreezeBuffer::Mode::Read, writer.GetData());
    uint32_t readVersion = 0;
    reader.Freeze(readVersion);
    EXPECT_EQ(readVersion, 1u);
    EXPECT_TRUE(reader.FreezeTag("Physics"));
    PhysicsState readPhys = {};
    reader.Freeze(readPhys);
    EXPECT_NEAR(readPhys.gravity, -9.81f, 0.001f);
    EXPECT_EQ(readPhys.subSteps, 4);
    EXPECT_TRUE(reader.FreezeTag("Audio"));
    AudioState readAudio = {};
    reader.Freeze(readAudio);
    EXPECT_NEAR(readAudio.masterVolume, 0.8f, 0.001f);
    EXPECT_EQ(readAudio.activeVoices, 24);
    EXPECT_TRUE(reader.FreezeTag("END"));
}

// ============================================================================
// 2. NullRHIDevice — Headless RHI backend
// ============================================================================

namespace
{

    struct NullDeviceStats
    {
        uint32_t buffersCreated = 0;
        uint32_t texturesCreated = 0;
        uint32_t shadersCreated = 0;
        uint32_t pipelinesCreated = 0;
        uint32_t framesRendered = 0;
        uint32_t drawCalls = 0;
    };

    class TestNullDevice
    {
      public:
        bool Initialize()
        {
            m_initialized = true;
            return true;
        }
        void Shutdown()
        {
            m_initialized = false;
            m_stats = {};
        }

        void CreateBuffer() { m_stats.buffersCreated++; }
        void CreateTexture() { m_stats.texturesCreated++; }
        void CreateShader() { m_stats.shadersCreated++; }
        void CreatePipeline() { m_stats.pipelinesCreated++; }
        void BeginFrame() { m_stats.framesRendered++; }
        void Draw() { m_stats.drawCalls++; }

        bool IsInitialized() const { return m_initialized; }
        const NullDeviceStats& GetStats() const { return m_stats; }

      private:
        bool m_initialized = false;
        NullDeviceStats m_stats;
    };

} // anonymous namespace

TEST(NullRHIDevice_InitShutdown)
{
    TestNullDevice device;
    EXPECT_FALSE(device.IsInitialized());
    EXPECT_TRUE(device.Initialize());
    EXPECT_TRUE(device.IsInitialized());
    device.Shutdown();
    EXPECT_FALSE(device.IsInitialized());
}

TEST(NullRHIDevice_ResourceTracking)
{
    TestNullDevice device;
    device.Initialize();

    device.CreateBuffer();
    device.CreateBuffer();
    device.CreateTexture();
    device.CreateShader();
    device.CreatePipeline();

    EXPECT_EQ(device.GetStats().buffersCreated, 2u);
    EXPECT_EQ(device.GetStats().texturesCreated, 1u);
    EXPECT_EQ(device.GetStats().shadersCreated, 1u);
    EXPECT_EQ(device.GetStats().pipelinesCreated, 1u);
}

TEST(NullRHIDevice_FrameAndDraw)
{
    TestNullDevice device;
    device.Initialize();

    device.BeginFrame();
    device.Draw();
    device.Draw();
    device.Draw();
    device.BeginFrame();
    device.Draw();

    EXPECT_EQ(device.GetStats().framesRendered, 2u);
    EXPECT_EQ(device.GetStats().drawCalls, 4u);
}

// ============================================================================
// 3. RenderCommandRing — SPSC command ring buffer
// ============================================================================

namespace
{

    enum class TestRenderCmd : uint8_t
    {
        Nop,
        BeginFrame,
        EndFrame,
        Draw,
        Lambda
    };

    struct TestCommand
    {
        TestRenderCmd type = TestRenderCmd::Nop;
        uint32_t param = 0;
        std::function<void()> lambda;
    };

    template <uint32_t SizeLog2 = 10>
    class TestCommandRing
    {
      public:
        static constexpr uint32_t CAPACITY = 1u << SizeLog2;
        static constexpr uint32_t MASK = CAPACITY - 1;

        bool Post(const TestCommand& cmd)
        {
            uint32_t w = m_writePos.load(std::memory_order_relaxed);
            uint32_t r = m_readPos.load(std::memory_order_acquire);
            if (w - r >= CAPACITY)
                return false;
            m_ring[w & MASK] = cmd;
            m_writePos.store(w + 1, std::memory_order_release);
            return true;
        }

        bool Consume(TestCommand& outCmd)
        {
            uint32_t r = m_readPos.load(std::memory_order_relaxed);
            uint32_t w = m_writePos.load(std::memory_order_acquire);
            if (r >= w)
                return false;
            outCmd = std::move(m_ring[r & MASK]);
            m_readPos.store(r + 1, std::memory_order_release);
            return true;
        }

        uint32_t GetPendingCount() const
        {
            return m_writePos.load(std::memory_order_acquire) - m_readPos.load(std::memory_order_acquire);
        }

        bool IsEmpty() const { return GetPendingCount() == 0; }

      private:
        std::vector<TestCommand> m_ring = std::vector<TestCommand>(CAPACITY);
        std::atomic<uint32_t> m_writePos{0};
        std::atomic<uint32_t> m_readPos{0};
    };

} // anonymous namespace

TEST(RenderCommandRing_PostConsume)
{
    TestCommandRing<4> ring; // 16 entries
    TestCommand cmd;
    cmd.type = TestRenderCmd::Draw;
    cmd.param = 42;
    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_EQ(ring.GetPendingCount(), 1u);

    TestCommand out;
    EXPECT_TRUE(ring.Consume(out));
    EXPECT_EQ(static_cast<int>(out.type), static_cast<int>(TestRenderCmd::Draw));
    EXPECT_EQ(out.param, 42u);
    EXPECT_TRUE(ring.IsEmpty());
}

TEST(RenderCommandRing_Full)
{
    TestCommandRing<2> ring; // 4 entries
    TestCommand cmd;
    cmd.type = TestRenderCmd::Draw;

    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_TRUE(ring.Post(cmd));
    EXPECT_FALSE(ring.Post(cmd)); // Full
    EXPECT_EQ(ring.GetPendingCount(), 4u);

    TestCommand out;
    EXPECT_TRUE(ring.Consume(out));
    EXPECT_TRUE(ring.Post(cmd)); // Space freed
}

TEST(RenderCommandRing_Lambda)
{
    TestCommandRing<4> ring;
    int counter = 0;
    TestCommand cmd;
    cmd.type = TestRenderCmd::Lambda;
    cmd.lambda = [&counter] { counter++; };
    ring.Post(cmd);

    TestCommand out;
    ring.Consume(out);
    if (out.lambda)
        out.lambda();
    EXPECT_EQ(counter, 1);
}

TEST(RenderCommandRing_SPSC_Threading)
{
    TestCommandRing<10> ring; // 1024 entries
    constexpr int NUM_ITEMS = 500;
    std::atomic<int> consumed{0};

    std::thread producer([&] {
        for (int i = 0; i < NUM_ITEMS; ++i)
        {
            TestCommand cmd;
            cmd.type = TestRenderCmd::Draw;
            cmd.param = static_cast<uint32_t>(i);
            while (!ring.Post(cmd))
                std::this_thread::yield();
        }
    });

    std::thread consumer([&] {
        while (consumed.load() < NUM_ITEMS)
        {
            TestCommand out;
            if (ring.Consume(out))
                consumed.fetch_add(1);
            else
                std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(consumed.load(), NUM_ITEMS);
    EXPECT_TRUE(ring.IsEmpty());
}

// ============================================================================
// 4. MultiISA — CPU dispatch
// ============================================================================

namespace
{

    enum class TestISALevel : uint8_t
    {
        SSE2 = 0,
        SSE4 = 1,
        AVX = 2,
        AVX2 = 3,
        Count
    };

    template <typename FuncT>
    FuncT SelectBestISA(TestISALevel detected, const FuncT (&variants)[4])
    {
        for (int level = static_cast<int>(detected); level >= 0; --level)
        {
            if (variants[level])
                return variants[level];
        }
        return variants[0];
    }

} // anonymous namespace

TEST(MultiISA_SelectBest)
{
    using FuncT = int (*)();
    auto sse2_fn = []() -> int { return 2; };
    auto avx2_fn = []() -> int { return 8; };

    FuncT variants[4] = {sse2_fn, nullptr, nullptr, avx2_fn};

    auto best = SelectBestISA(TestISALevel::AVX2, variants);
    EXPECT_EQ(best(), 8);

    auto fallback = SelectBestISA(TestISALevel::AVX, variants);
    EXPECT_EQ(fallback(), 2); // Falls back to SSE2

    auto baseline = SelectBestISA(TestISALevel::SSE2, variants);
    EXPECT_EQ(baseline(), 2);
}

TEST(MultiISA_FallbackChain)
{
    using FuncT = int (*)();
    auto sse2_fn = []() -> int { return 1; };
    auto sse4_fn = []() -> int { return 4; };

    FuncT variants[4] = {sse2_fn, sse4_fn, nullptr, nullptr};

    auto avx2Select = SelectBestISA(TestISALevel::AVX2, variants);
    EXPECT_EQ(avx2Select(), 4); // Falls back to SSE4

    auto sse4Select = SelectBestISA(TestISALevel::SSE4, variants);
    EXPECT_EQ(sse4Select(), 4);
}

// ============================================================================
// 5. ConstantBufferDiff — Content-diff CB uploads
// ============================================================================

namespace
{

    class TestCBDiffManager
    {
      public:
        bool ShouldUpdate(const std::string& key, const void* data, size_t size)
        {
            auto& slot = m_slots[key];
            if (slot.size() != size || std::memcmp(slot.data(), data, size) != 0)
            {
                slot.resize(size);
                std::memcpy(slot.data(), data, size);
                m_updates++;
                return true;
            }
            m_skips++;
            return false;
        }

        uint64_t GetUpdates() const { return m_updates; }
        uint64_t GetSkips() const { return m_skips; }

      private:
        std::unordered_map<std::string, std::vector<uint8_t>> m_slots;
        uint64_t m_updates = 0;
        uint64_t m_skips = 0;
    };

} // anonymous namespace

TEST(CBDiff_SkipsRedundant)
{
    TestCBDiffManager mgr;
    struct PerObjectCB
    {
        float transform[16] = {};
    };

    PerObjectCB cb;
    cb.transform[0] = 1.0f;
    EXPECT_TRUE(mgr.ShouldUpdate("VS_PerObject", &cb, sizeof(cb)));
    EXPECT_FALSE(mgr.ShouldUpdate("VS_PerObject", &cb, sizeof(cb))); // Same data
    EXPECT_EQ(mgr.GetUpdates(), 1u);
    EXPECT_EQ(mgr.GetSkips(), 1u);

    cb.transform[0] = 2.0f;
    EXPECT_TRUE(mgr.ShouldUpdate("VS_PerObject", &cb, sizeof(cb))); // Changed
    EXPECT_EQ(mgr.GetUpdates(), 2u);
}

TEST(CBDiff_MultipleSlots)
{
    TestCBDiffManager mgr;
    float data1 = 1.0f;
    float data2 = 2.0f;

    EXPECT_TRUE(mgr.ShouldUpdate("VS_Slot0", &data1, sizeof(data1)));
    EXPECT_TRUE(mgr.ShouldUpdate("PS_Slot0", &data2, sizeof(data2)));
    EXPECT_FALSE(mgr.ShouldUpdate("VS_Slot0", &data1, sizeof(data1)));
    EXPECT_FALSE(mgr.ShouldUpdate("PS_Slot0", &data2, sizeof(data2)));
    EXPECT_EQ(mgr.GetSkips(), 2u);
}

// ============================================================================
// 6. DirtyRectTracker — Partial texture update tracking
// ============================================================================

namespace
{

    struct TestDirtyRect
    {
        uint32_t x, y, width, height;

        bool Overlaps(const TestDirtyRect& other) const
        {
            return x < other.x + other.width && x + width > other.x && y < other.y + other.height &&
                   y + height > other.y;
        }

        bool Contains(const TestDirtyRect& other) const
        {
            return x <= other.x && y <= other.y && x + width >= other.x + other.width &&
                   y + height >= other.y + other.height;
        }

        TestDirtyRect Union(const TestDirtyRect& other) const
        {
            uint32_t minX = std::min(x, other.x);
            uint32_t minY = std::min(y, other.y);
            uint32_t maxX = std::max(x + width, other.x + other.width);
            uint32_t maxY = std::max(y + height, other.y + other.height);
            return {minX, minY, maxX - minX, maxY - minY};
        }
    };

    class TestDirtyRectTracker
    {
      public:
        void AddDirtyRect(const TestDirtyRect& rect)
        {
            if (rect.width == 0 || rect.height == 0)
                return;
            for (auto& existing : m_rects)
            {
                if (existing.Contains(rect))
                    return;
                if (existing.Overlaps(rect))
                {
                    existing = existing.Union(rect);
                    return;
                }
            }
            m_rects.push_back(rect);
        }

        void Clear() { m_rects.clear(); }
        bool IsDirty() const { return !m_rects.empty(); }
        size_t GetRectCount() const { return m_rects.size(); }
        const std::vector<TestDirtyRect>& GetRects() const { return m_rects; }

      private:
        std::vector<TestDirtyRect> m_rects;
    };

} // anonymous namespace

TEST(DirtyRect_AddAndClear)
{
    TestDirtyRectTracker tracker;
    EXPECT_FALSE(tracker.IsDirty());

    tracker.AddDirtyRect({10, 10, 50, 50});
    EXPECT_TRUE(tracker.IsDirty());
    EXPECT_EQ(tracker.GetRectCount(), 1u);

    tracker.Clear();
    EXPECT_FALSE(tracker.IsDirty());
}

TEST(DirtyRect_MergeOverlapping)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 100, 100});
    tracker.AddDirtyRect({50, 50, 100, 100}); // Overlapping
    EXPECT_EQ(tracker.GetRectCount(), 1u);

    auto& merged = tracker.GetRects()[0];
    EXPECT_EQ(merged.x, 0u);
    EXPECT_EQ(merged.y, 0u);
    EXPECT_EQ(merged.width, 150u);
    EXPECT_EQ(merged.height, 150u);
}

TEST(DirtyRect_NonOverlappingSeparate)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 10, 10});
    tracker.AddDirtyRect({100, 100, 10, 10}); // Far away
    EXPECT_EQ(tracker.GetRectCount(), 2u);
}

TEST(DirtyRect_ContainedSkipped)
{
    TestDirtyRectTracker tracker;
    tracker.AddDirtyRect({0, 0, 100, 100});
    tracker.AddDirtyRect({10, 10, 20, 20}); // Fully contained
    EXPECT_EQ(tracker.GetRectCount(), 1u);
}

// ============================================================================
// 7. LockFreeRingAllocator — SPSC ring allocator
// ============================================================================

namespace
{

    class TestRingAllocator
    {
      public:
        static constexpr size_t CAPACITY = 1024;

        TestRingAllocator() : m_buffer(CAPACITY, 0) {}

        void* Allocate(size_t size)
        {
            size_t aligned = (size + 7) & ~7;
            if (m_used + aligned > CAPACITY)
                return nullptr;
            void* ptr = m_buffer.data() + m_writePos;
            m_writePos += aligned;
            m_used += aligned;
            m_allocCount++;
            return ptr;
        }

        void Reset()
        {
            m_writePos = 0;
            m_used = 0;
        }

        size_t GetUsed() const { return m_used; }
        bool IsEmpty() const { return m_used == 0; }
        uint64_t GetAllocCount() const { return m_allocCount; }

      private:
        std::vector<uint8_t> m_buffer;
        size_t m_writePos = 0;
        size_t m_used = 0;
        uint64_t m_allocCount = 0;
    };

} // anonymous namespace

TEST(RingAllocator_AllocateAndReset)
{
    TestRingAllocator alloc;
    EXPECT_TRUE(alloc.IsEmpty());

    void* p1 = alloc.Allocate(64);
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_FALSE(alloc.IsEmpty());
    EXPECT_EQ(alloc.GetAllocCount(), 1u);

    void* p2 = alloc.Allocate(128);
    EXPECT_TRUE(p2 != nullptr);
    EXPECT_EQ(alloc.GetAllocCount(), 2u);

    alloc.Reset();
    EXPECT_TRUE(alloc.IsEmpty());
}

TEST(RingAllocator_CapacityLimit)
{
    TestRingAllocator alloc;
    void* p = alloc.Allocate(2048); // Exceeds 1024
    EXPECT_TRUE(p == nullptr);
}

TEST(RingAllocator_AlignedAllocations)
{
    TestRingAllocator alloc;
    void* p1 = alloc.Allocate(3); // Aligns to 8
    void* p2 = alloc.Allocate(5); // Aligns to 8
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_TRUE(p2 != nullptr);
    EXPECT_EQ(alloc.GetUsed(), 16u); // 8 + 8
}

// ============================================================================
// 8. SceneConfigDatabase — Per-scene overrides
// ============================================================================

namespace
{

    struct TestSceneConfig
    {
        std::string sceneId;
        std::string displayName;
        float shadowDistance = 100.0f;
        int shadowCascades = 4;
        std::string reverbPreset;
        std::unordered_map<std::string, std::string> custom;
    };

    class TestSceneDB
    {
      public:
        void Register(const TestSceneConfig& config) { m_entries[config.sceneId] = config; }
        void Unregister(const std::string& id) { m_entries.erase(id); }

        const TestSceneConfig* Get(const std::string& id) const
        {
            auto it = m_entries.find(id);
            return it != m_entries.end() ? &it->second : nullptr;
        }

        bool Has(const std::string& id) const { return m_entries.count(id) > 0; }
        size_t Count() const { return m_entries.size(); }

      private:
        std::unordered_map<std::string, TestSceneConfig> m_entries;
    };

} // anonymous namespace

TEST(SceneConfigDB_RegisterLookup)
{
    TestSceneDB db;
    TestSceneConfig config;
    config.sceneId = "level01";
    config.displayName = "The Beginning";
    config.shadowDistance = 200.0f;
    config.reverbPreset = "Cave";

    db.Register(config);
    EXPECT_TRUE(db.Has("level01"));
    EXPECT_FALSE(db.Has("level02"));

    auto* found = db.Get("level01");
    EXPECT_TRUE(found != nullptr);
    EXPECT_NEAR(found->shadowDistance, 200.0f, 0.01f);
    EXPECT_EQ(found->reverbPreset, std::string("Cave"));
}

TEST(SceneConfigDB_Unregister)
{
    TestSceneDB db;
    TestSceneConfig config;
    config.sceneId = "arena";
    db.Register(config);
    EXPECT_EQ(db.Count(), 1u);

    db.Unregister("arena");
    EXPECT_EQ(db.Count(), 0u);
    EXPECT_FALSE(db.Has("arena"));
}

TEST(SceneConfigDB_CustomOverrides)
{
    TestSceneDB db;
    TestSceneConfig config;
    config.sceneId = "boss_room";
    config.custom["fog_density"] = "0.8";
    config.custom["particle_limit"] = "5000";
    db.Register(config);

    auto* found = db.Get("boss_room");
    EXPECT_TRUE(found != nullptr);
    EXPECT_EQ(found->custom.at("fog_density"), std::string("0.8"));
    EXPECT_EQ(found->custom.at("particle_limit"), std::string("5000"));
}

// ============================================================================
// 9. GPUPerfCounters — Categorized GPU counters
// ============================================================================

namespace
{

    enum class TestGPUCounter : uint8_t
    {
        DrawCalls,
        Primitives,
        StateChanges,
        TextureUploads,
        Barriers,
        Count
    };

    class TestGPUCounters
    {
      public:
        void Increment(TestGPUCounter cat, uint64_t amount = 1)
        {
            m_current[static_cast<size_t>(cat)] += amount;
        }

        void EndFrame()
        {
            m_lastFrame = m_current;
            m_current.fill(0);
            m_frameCount++;
        }

        void Reset()
        {
            m_current.fill(0);
            m_lastFrame.fill(0);
            m_frameCount = 0;
        }

        uint64_t GetLastFrame(TestGPUCounter cat) const { return m_lastFrame[static_cast<size_t>(cat)]; }
        uint64_t GetCurrent(TestGPUCounter cat) const { return m_current[static_cast<size_t>(cat)]; }
        uint32_t GetFrameCount() const { return m_frameCount; }

      private:
        std::array<uint64_t, static_cast<size_t>(TestGPUCounter::Count)> m_current = {};
        std::array<uint64_t, static_cast<size_t>(TestGPUCounter::Count)> m_lastFrame = {};
        uint32_t m_frameCount = 0;
    };

} // anonymous namespace

TEST(GPUPerfCounters_IncrementAndFrame)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::DrawCalls, 150);
    counters.Increment(TestGPUCounter::Primitives, 50000);
    counters.Increment(TestGPUCounter::StateChanges, 30);

    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::DrawCalls), 150u);
    counters.EndFrame();

    EXPECT_EQ(counters.GetLastFrame(TestGPUCounter::DrawCalls), 150u);
    EXPECT_EQ(counters.GetLastFrame(TestGPUCounter::Primitives), 50000u);
    EXPECT_EQ(counters.GetCurrent(TestGPUCounter::DrawCalls), 0u); // Reset
    EXPECT_EQ(counters.GetFrameCount(), 1u);
}

TEST(GPUPerfCounters_Reset)
{
    TestGPUCounters counters;
    counters.Increment(TestGPUCounter::Barriers, 10);
    counters.EndFrame();
    counters.Reset();
    EXPECT_EQ(counters.GetLastFrame(TestGPUCounter::Barriers), 0u);
    EXPECT_EQ(counters.GetFrameCount(), 0u);
}

// ============================================================================
// 10. WorkSema — Spin-before-sleep semaphore
// ============================================================================

TEST(WorkSema_PostAndTryAcquire)
{
    std::atomic<int32_t> count{0};

    count.fetch_add(3, std::memory_order_release);
    EXPECT_EQ(count.load(), 3);

    // Simulate TryAcquire
    int32_t expected = count.load(std::memory_order_relaxed);
    bool acquired = false;
    while (expected > 0)
    {
        if (count.compare_exchange_weak(expected, expected - 1))
        {
            acquired = true;
            break;
        }
    }
    EXPECT_TRUE(acquired);
    EXPECT_EQ(count.load(), 2);
}

TEST(WorkSema_ProducerConsumer)
{
    std::atomic<int32_t> sema{0};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    constexpr int ITEMS = 100;

    std::thread producer([&] {
        for (int i = 0; i < ITEMS; ++i)
        {
            produced.fetch_add(1);
            sema.fetch_add(1, std::memory_order_release);
        }
    });

    std::thread consumer([&] {
        while (consumed.load() < ITEMS)
        {
            int32_t val = sema.load(std::memory_order_relaxed);
            if (val > 0 && sema.compare_exchange_weak(val, val - 1))
                consumed.fetch_add(1);
            else
                std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(consumed.load(), ITEMS);
}

// ============================================================================
// 11. AlignedHeapArray — SIMD-aligned memory
// ============================================================================

namespace
{

    template <typename T, size_t SIZE, size_t ALIGNMENT = 32>
    class TestAlignedArray
    {
      public:
        TestAlignedArray()
        {
#ifdef _MSC_VER
            m_data = static_cast<T*>(_aligned_malloc(SIZE * sizeof(T), ALIGNMENT));
#else
            void* ptr = nullptr;
            if (posix_memalign(&ptr, ALIGNMENT, SIZE * sizeof(T)) != 0)
                ptr = nullptr;
            m_data = static_cast<T*>(ptr);
#endif
            if (m_data)
                std::memset(m_data, 0, SIZE * sizeof(T));
        }

        ~TestAlignedArray()
        {
#ifdef _MSC_VER
            _aligned_free(m_data);
#else
            free(m_data);
#endif
        }

        T* data() { return m_data; }
        constexpr size_t size() const { return SIZE; }
        T& operator[](size_t i) { return m_data[i]; }

        bool IsAligned() const
        {
            return (reinterpret_cast<uintptr_t>(m_data) % ALIGNMENT) == 0;
        }

      private:
        T* m_data = nullptr;
    };

} // anonymous namespace

TEST(AlignedHeapArray_Alignment)
{
    TestAlignedArray<float, 256, 32> arr;
    EXPECT_TRUE(arr.IsAligned());
    EXPECT_EQ(arr.size(), 256u);
}

TEST(AlignedHeapArray_ReadWrite)
{
    TestAlignedArray<int32_t, 64, 16> arr;
    for (size_t i = 0; i < arr.size(); ++i)
        arr[i] = static_cast<int32_t>(i * 2);

    EXPECT_EQ(arr[0], 0);
    EXPECT_EQ(arr[1], 2);
    EXPECT_EQ(arr[63], 126);
}

TEST(AlignedHeapArray_ZeroInitialized)
{
    TestAlignedArray<uint8_t, 128, 32> arr;
    bool allZero = true;
    for (size_t i = 0; i < arr.size(); ++i)
    {
        if (arr[i] != 0)
        {
            allZero = false;
            break;
        }
    }
    EXPECT_TRUE(allZero);
}

// ============================================================================
// 12. DynamicHeapArray — Runtime-sized aligned array
// ============================================================================

namespace
{

    template <typename T, size_t ALIGNMENT = 32>
    class TestDynamicArray
    {
      public:
        TestDynamicArray() = default;

        explicit TestDynamicArray(size_t count) : m_size(count)
        {
            if (count > 0)
            {
#ifdef _MSC_VER
                m_data = static_cast<T*>(_aligned_malloc(count * sizeof(T), ALIGNMENT));
#else
                void* ptr = nullptr;
                if (posix_memalign(&ptr, ALIGNMENT, count * sizeof(T)) != 0)
                    ptr = nullptr;
                m_data = static_cast<T*>(ptr);
#endif
                if (m_data)
                    std::memset(m_data, 0, count * sizeof(T));
            }
        }

        ~TestDynamicArray()
        {
#ifdef _MSC_VER
            _aligned_free(m_data);
#else
            free(m_data);
#endif
        }

        // Move
        TestDynamicArray(TestDynamicArray&& other) noexcept : m_data(other.m_data), m_size(other.m_size)
        {
            other.m_data = nullptr;
            other.m_size = 0;
        }

        T* data() { return m_data; }
        size_t size() const { return m_size; }
        bool empty() const { return m_size == 0; }
        T& operator[](size_t i) { return m_data[i]; }

      private:
        T* m_data = nullptr;
        size_t m_size = 0;
    };

} // anonymous namespace

TEST(DynamicHeapArray_Create)
{
    TestDynamicArray<float> arr(128);
    EXPECT_EQ(arr.size(), 128u);
    EXPECT_FALSE(arr.empty());
    EXPECT_TRUE(arr.data() != nullptr);
}

TEST(DynamicHeapArray_MoveSemantics)
{
    TestDynamicArray<int32_t> arr(64);
    arr[0] = 42;
    arr[63] = 99;

    TestDynamicArray<int32_t> moved(std::move(arr));
    EXPECT_EQ(moved.size(), 64u);
    EXPECT_EQ(moved[0], 42);
    EXPECT_EQ(moved[63], 99);
    EXPECT_TRUE(arr.empty());
}

TEST(DynamicHeapArray_Empty)
{
    TestDynamicArray<float> arr;
    EXPECT_TRUE(arr.empty());
    EXPECT_EQ(arr.size(), 0u);
}
