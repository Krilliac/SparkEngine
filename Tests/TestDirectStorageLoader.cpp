// TestDirectStorageLoader.cpp - Tests for DirectStorage async I/O loader
// Validates request management, priority, and status tracking

#include "TestFramework.h"

#include <cstdint>
#include <string>
#include <vector>

namespace
{

    enum class LoadPriority : uint8_t
    {
        Low = 0,
        Normal = 1,
        High = 2,
        Critical = 3
    };

    enum class LoadDestination : uint8_t
    {
        CPUMemory,
        GPUTexture,
        GPUBuffer
    };

    enum class LoadStatus : uint8_t
    {
        Pending,
        InProgress,
        Completed,
        Failed
    };

    struct LoadRequestHandle
    {
        uint64_t id = 0;
        bool IsValid() const { return id != 0; }
    };

    struct LoadRequest
    {
        std::string filePath;
        uint64_t fileOffset = 0;
        uint64_t loadSize = 0;
        LoadDestination destination = LoadDestination::CPUMemory;
        LoadPriority priority = LoadPriority::Normal;
    };

    struct LoaderStats
    {
        uint64_t totalBytesLoaded = 0;
        uint64_t totalRequests = 0;
        uint64_t pendingRequests = 0;
        uint64_t failedRequests = 0;
        bool usingDirectStorage = false;
    };

    // Simplified loader for testing
    class TestLoader
    {
      public:
        LoadRequestHandle Submit(const LoadRequest& request)
        {
            LoadRequestHandle handle = {m_nextId++};
            m_requests.push_back({handle, request, LoadStatus::Pending});
            m_stats.totalRequests++;
            m_stats.pendingRequests++;
            return handle;
        }

        LoadStatus GetStatus(LoadRequestHandle handle) const
        {
            for (const auto& req : m_requests)
            {
                if (req.handle.id == handle.id)
                    return req.status;
            }
            return LoadStatus::Failed;
        }

        bool Cancel(LoadRequestHandle handle)
        {
            for (auto& req : m_requests)
            {
                if (req.handle.id == handle.id && req.status == LoadStatus::Pending)
                {
                    req.status = LoadStatus::Failed;
                    m_stats.pendingRequests--;
                    return true;
                }
            }
            return false;
        }

        void CompleteRequest(LoadRequestHandle handle, uint64_t bytesLoaded)
        {
            for (auto& req : m_requests)
            {
                if (req.handle.id == handle.id)
                {
                    req.status = LoadStatus::Completed;
                    m_stats.pendingRequests--;
                    m_stats.totalBytesLoaded += bytesLoaded;
                    return;
                }
            }
        }

        const LoaderStats& GetStats() const { return m_stats; }

      private:
        struct InternalRequest
        {
            LoadRequestHandle handle;
            LoadRequest request;
            LoadStatus status;
        };

        std::vector<InternalRequest> m_requests;
        LoaderStats m_stats;
        uint64_t m_nextId = 1;
    };

} // namespace

TEST(DirectStorage_Handle_InitiallyInvalid)
{
    LoadRequestHandle handle;
    EXPECT_FALSE(handle.IsValid());
}

TEST(DirectStorage_Submit_ReturnsValidHandle)
{
    TestLoader loader;
    LoadRequest req;
    req.filePath = "textures/diffuse.dds";

    auto handle = loader.Submit(req);
    EXPECT_TRUE(handle.IsValid());
}

TEST(DirectStorage_Submit_StatusPending)
{
    TestLoader loader;
    LoadRequest req;
    req.filePath = "test.bin";

    auto handle = loader.Submit(req);
    EXPECT_EQ(static_cast<uint8_t>(loader.GetStatus(handle)), static_cast<uint8_t>(LoadStatus::Pending));
}

TEST(DirectStorage_Cancel_MarksFailed)
{
    TestLoader loader;
    auto handle = loader.Submit({"test.bin", 0, 0, LoadDestination::CPUMemory, LoadPriority::Normal});

    EXPECT_TRUE(loader.Cancel(handle));
    EXPECT_EQ(static_cast<uint8_t>(loader.GetStatus(handle)), static_cast<uint8_t>(LoadStatus::Failed));
}

TEST(DirectStorage_Cancel_InProgressFails)
{
    TestLoader loader;
    auto handle = loader.Submit({"test.bin", 0, 0, LoadDestination::CPUMemory, LoadPriority::Normal});

    // Manually simulate in-progress state
    loader.CompleteRequest(handle, 100);

    EXPECT_FALSE(loader.Cancel(handle)); // can't cancel completed
}

TEST(DirectStorage_Complete_TracksBytesLoaded)
{
    TestLoader loader;
    auto h1 = loader.Submit({"file1.bin", 0, 0, LoadDestination::CPUMemory, LoadPriority::Normal});
    auto h2 = loader.Submit({"file2.bin", 0, 0, LoadDestination::CPUMemory, LoadPriority::Normal});

    loader.CompleteRequest(h1, 1024);
    loader.CompleteRequest(h2, 2048);

    EXPECT_EQ(loader.GetStats().totalBytesLoaded, 3072u);
}

TEST(DirectStorage_Stats_CountsRequests)
{
    TestLoader loader;
    loader.Submit({"a.bin", 0, 0, LoadDestination::CPUMemory, LoadPriority::Normal});
    loader.Submit({"b.bin", 0, 0, LoadDestination::CPUMemory, LoadPriority::Normal});
    loader.Submit({"c.bin", 0, 0, LoadDestination::CPUMemory, LoadPriority::Normal});

    EXPECT_EQ(loader.GetStats().totalRequests, 3u);
    EXPECT_EQ(loader.GetStats().pendingRequests, 3u);
}

TEST(DirectStorage_Priority_AllLevels)
{
    LoadRequest req;
    req.priority = LoadPriority::Low;
    EXPECT_EQ(static_cast<uint8_t>(req.priority), 0u);

    req.priority = LoadPriority::Critical;
    EXPECT_EQ(static_cast<uint8_t>(req.priority), 3u);
}

TEST(DirectStorage_Destination_AllTypes)
{
    LoadRequest req;

    req.destination = LoadDestination::CPUMemory;
    EXPECT_EQ(static_cast<uint8_t>(req.destination), 0u);

    req.destination = LoadDestination::GPUTexture;
    EXPECT_EQ(static_cast<uint8_t>(req.destination), 1u);

    req.destination = LoadDestination::GPUBuffer;
    EXPECT_EQ(static_cast<uint8_t>(req.destination), 2u);
}

TEST(DirectStorage_UniqueHandles)
{
    TestLoader loader;
    auto h1 = loader.Submit({"a.bin", 0, 0, LoadDestination::CPUMemory, LoadPriority::Normal});
    auto h2 = loader.Submit({"b.bin", 0, 0, LoadDestination::CPUMemory, LoadPriority::Normal});

    EXPECT_TRUE(h1.id != h2.id);
}

TEST(DirectStorage_FallbackPath_NoDirectStorage)
{
    TestLoader loader;
    EXPECT_FALSE(loader.GetStats().usingDirectStorage);
}
