/**
 * @file TestAdvancedAssetPipeline.cpp
 * @brief Production-linked completion, cancellation, and fail-closed tests.
 */

#include "TestFramework.h"
#include "AssetPipeline/AdvancedAssetPipeline.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;

    struct TempAssets
    {
        TempAssets()
        {
            static std::atomic<uint64_t> sequence{0};
            root = fs::temp_directory_path() /
                   ("spark_advanced_asset_" + std::to_string(++sequence) + "_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(root);
        }

        ~TempAssets()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        fs::path Write(const std::string& name) const
        {
            const fs::path path = root / name;
            std::ofstream stream(path, std::ios::binary);
            stream << "asset";
            return path;
        }

        fs::path root;
    };

    struct GateState
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false;
        bool release = false;
        int calls = 0;
        std::thread::id workerThread;
    };

    class GateProcessor final : public SparkEditor::AssetProcessor
    {
      public:
        explicit GateProcessor(std::shared_ptr<GateState> state) : m_state(std::move(state)) {}

        std::string GetName() const override { return "GateProcessor"; }
        std::vector<std::string> GetSupportedExtensions() const override { return {".assettest"}; }
        SparkEditor::AssetType GetAssetType() const override { return SparkEditor::AssetType::UNKNOWN; }

        bool Process(SparkEditor::AssetMetadata& metadata, const SparkEditor::AssetImportSettings&,
                     std::function<void(float)> progressCallback) override
        {
            bool firstCall = false;
            {
                std::unique_lock<std::mutex> lock(m_state->mutex);
                firstCall = ++m_state->calls == 1;
                m_state->workerThread = std::this_thread::get_id();
                if (firstCall)
                {
                    m_state->entered = true;
                    m_state->condition.notify_all();
                    if (!m_state->condition.wait_for(lock, 5s, [this] { return m_state->release; }))
                    {
                        metadata.status = SparkEditor::ProcessingStatus::FAILED;
                        metadata.errorMessage = "Test processor gate timed out";
                        return false;
                    }
                }
            }

            if (progressCallback)
                progressCallback(1.0f);
            metadata.processedFilePath = metadata.sourceFilePath;
            metadata.processedFileSize = fs::file_size(metadata.sourceFilePath);
            metadata.processorName = GetName();
            metadata.type = SparkEditor::AssetType::UNKNOWN;
            metadata.status = SparkEditor::ProcessingStatus::COMPLETED;
            return true;
        }

        bool GenerateThumbnail(const SparkEditor::AssetMetadata&, int) override { return false; }
        bool Validate(const SparkEditor::AssetMetadata& metadata) override
        {
            return fs::exists(metadata.sourceFilePath);
        }

      private:
        std::shared_ptr<GateState> m_state;
    };

    bool WaitForGate(const std::shared_ptr<GateState>& state)
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        return state->condition.wait_for(lock, 2s, [&] { return state->entered; });
    }

    void ReleaseGate(const std::shared_ptr<GateState>& state)
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
        state->condition.notify_all();
    }

    template <typename Predicate> bool WaitUntil(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(2ms);
        return predicate();
    }

    SparkEditor::AssetImportSettings PassThroughSettings()
    {
        SparkEditor::AssetImportSettings settings;
        settings.textureSettings.generateMipMaps = false;
        settings.meshSettings.optimizeMesh = false;
        settings.meshSettings.weldVertices = false;
        settings.meshSettings.generateNormals = false;
        settings.meshSettings.generateTangents = false;
        settings.meshSettings.generateLightmapUVs = false;
        return settings;
    }
} // namespace

TEST(AdvancedAssetPipeline_ReentrantBatchCompletionRunsOutsideLocksExactlyOnce)
{
    TempAssets assets;
    const fs::path source = assets.Write("reentrant.assettest");
    auto gate = std::make_shared<GateState>();

    SparkEditor::AdvancedAssetPipeline pipeline;
    pipeline.SetFileSystemMonitoring(false);
    pipeline.SetProcessingThreadCount(1);
    EXPECT_TRUE(pipeline.Initialize());
    pipeline.RegisterProcessor(std::make_unique<GateProcessor>(gate));

    std::atomic<uint32_t> operationID{0};
    std::atomic<int> progressCount{0};
    std::atomic<int> completionCount{0};
    std::atomic<bool> snapshotWasTerminal{false};
    std::thread::id progressThread;
    std::thread::id callbackThread;
    const std::thread::id updateThread = std::this_thread::get_id();
    const uint32_t id = pipeline.ProcessAssetsBatch(
        {source.string()}, PassThroughSettings(),
        [&](float progress)
        {
            SparkEditor::BatchOperation snapshot;
            snapshotWasTerminal.store(pipeline.GetBatchOperation(operationID.load(), snapshot) &&
                                      snapshot.status == SparkEditor::ProcessingStatus::COMPLETED && progress == 1.0f);
            progressThread = std::this_thread::get_id();
            ++progressCount;
        },
        [&]
        {
            SparkEditor::BatchOperation snapshot;
            const bool found = pipeline.GetBatchOperation(operationID.load(), snapshot);
            snapshotWasTerminal.store(found && !snapshot.isActive &&
                                      snapshot.status == SparkEditor::ProcessingStatus::COMPLETED);
            callbackThread = std::this_thread::get_id();
            ++completionCount;
        });
    operationID.store(id);

    EXPECT_TRUE(WaitForGate(gate));
    ReleaseGate(gate);
    EXPECT_TRUE(WaitUntil(
        [&]
        {
            SparkEditor::BatchOperation snapshot;
            return pipeline.GetBatchOperation(id, snapshot) && !snapshot.isActive;
        }));
    EXPECT_EQ(progressCount.load(), 0);
    EXPECT_EQ(completionCount.load(), 0);
    pipeline.Update(0.0f);
    EXPECT_EQ(progressCount.load(), 1);
    EXPECT_EQ(completionCount.load(), 1);
    EXPECT_TRUE(snapshotWasTerminal.load());
    EXPECT_TRUE(progressThread == updateThread);
    EXPECT_TRUE(callbackThread == updateThread);
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        EXPECT_FALSE(gate->workerThread == callbackThread);
    }
    EXPECT_FALSE(pipeline.CancelBatchOperation(id));
    pipeline.Update(0.0f);
    EXPECT_EQ(completionCount.load(), 1);
}

TEST(AdvancedAssetPipeline_CancelKeepsBatchLifetimeAndUnrelatedSamePathJob)
{
    TempAssets assets;
    const fs::path first = assets.Write("first.assettest");
    const fs::path second = assets.Write("second.assettest");
    auto gate = std::make_shared<GateState>();

    SparkEditor::AdvancedAssetPipeline pipeline;
    pipeline.SetFileSystemMonitoring(false);
    pipeline.SetProcessingThreadCount(1);
    EXPECT_TRUE(pipeline.Initialize());
    pipeline.RegisterProcessor(std::make_unique<GateProcessor>(gate));

    std::atomic<int> batchCompletionCount{0};
    const uint32_t id = pipeline.ProcessAssetsBatch({first.string(), second.string()}, PassThroughSettings(), nullptr,
                                                    [&] { ++batchCompletionCount; });

    std::atomic<int> standaloneCompletionCount{0};
    std::atomic<bool> standaloneCompleted{false};
    EXPECT_TRUE(pipeline.ProcessAsset(first.string(), PassThroughSettings(),
                                      [&](const SparkEditor::AssetMetadata& metadata)
                                      {
                                          standaloneCompleted.store(metadata.status ==
                                                                    SparkEditor::ProcessingStatus::COMPLETED);
                                          ++standaloneCompletionCount;
                                      }));

    EXPECT_TRUE(WaitForGate(gate));
    EXPECT_TRUE(pipeline.CancelBatchOperation(id));
    EXPECT_EQ(batchCompletionCount.load(), 0);

    SparkEditor::BatchOperation cancelled;
    EXPECT_TRUE(pipeline.GetBatchOperation(id, cancelled));
    EXPECT_FALSE(cancelled.isActive);
    EXPECT_TRUE(cancelled.status == SparkEditor::ProcessingStatus::CANCELLED);
    EXPECT_TRUE(cancelled.completionDispatched);

    pipeline.Update(0.0f);
    EXPECT_EQ(batchCompletionCount.load(), 1);

    ReleaseGate(gate);
    EXPECT_TRUE(WaitUntil([&] { return standaloneCompletionCount.load() == 1; }));
    EXPECT_TRUE(standaloneCompleted.load());
    pipeline.Update(0.0f);
    EXPECT_EQ(batchCompletionCount.load(), 1);
    pipeline.Shutdown();
    EXPECT_EQ(batchCompletionCount.load(), 1);
}

TEST(AdvancedAssetPipeline_ShutdownDrainsQueuedTerminalNotificationOnce)
{
    TempAssets assets;
    const fs::path source = assets.Write("shutdown.assettest");
    auto gate = std::make_shared<GateState>();

    SparkEditor::AdvancedAssetPipeline pipeline;
    pipeline.SetFileSystemMonitoring(false);
    pipeline.SetProcessingThreadCount(1);
    EXPECT_TRUE(pipeline.Initialize());
    pipeline.RegisterProcessor(std::make_unique<GateProcessor>(gate));

    std::atomic<int> completionCount{0};
    const std::thread::id shutdownThread = std::this_thread::get_id();
    std::thread::id callbackThread;
    const uint32_t id = pipeline.ProcessAssetsBatch({source.string()}, PassThroughSettings(), nullptr,
                                                    [&]
                                                    {
                                                        callbackThread = std::this_thread::get_id();
                                                        ++completionCount;
                                                    });

    EXPECT_TRUE(WaitForGate(gate));
    ReleaseGate(gate);
    EXPECT_TRUE(WaitUntil(
        [&]
        {
            SparkEditor::BatchOperation snapshot;
            return pipeline.GetBatchOperation(id, snapshot) && !snapshot.isActive;
        }));
    EXPECT_EQ(completionCount.load(), 0);
    pipeline.Shutdown();
    EXPECT_EQ(completionCount.load(), 1);
    EXPECT_TRUE(callbackThread == shutdownThread);
}

TEST(AdvancedAssetPipeline_UnsupportedTransformAndImporterFailClosed)
{
    TempAssets assets;
    const fs::path texturePath = assets.Write("unsupported_transform.png");
    const fs::path outputDirectory = assets.root / "processed";

    SparkEditor::TextureProcessor textureProcessor;
    SparkEditor::AssetMetadata textureMetadata;
    textureMetadata.sourceFilePath = texturePath.string();
    SparkEditor::AssetImportSettings transformSettings = PassThroughSettings();
    transformSettings.outputDirectory = outputDirectory.string();
    transformSettings.textureSettings.format = SparkEditor::AssetImportSettings::TextureSettings::DXT1;

    EXPECT_FALSE(textureProcessor.Process(textureMetadata, transformSettings, nullptr));
    EXPECT_TRUE(textureMetadata.status == SparkEditor::ProcessingStatus::FAILED);
    EXPECT_FALSE(textureMetadata.errorMessage.empty());
    EXPECT_FALSE(fs::exists(outputDirectory / texturePath.filename()));
    EXPECT_FALSE(textureProcessor.GenerateThumbnail(textureMetadata, 64));

    SparkEditor::AssetMetadata defaultMetadata;
    EXPECT_TRUE(defaultMetadata.type == SparkEditor::AssetType::UNKNOWN);
    defaultMetadata.sourceFilePath = texturePath.string();
    SparkEditor::AssetImportSettings defaultSettings;
    defaultSettings.outputDirectory = (assets.root / "default_processed").string();
    EXPECT_TRUE(textureProcessor.Process(defaultMetadata, defaultSettings, nullptr));
    EXPECT_TRUE(defaultMetadata.status == SparkEditor::ProcessingStatus::COMPLETED);
    EXPECT_TRUE(fs::exists(defaultMetadata.processedFilePath));

    SparkEditor::AdvancedAssetPipeline pipeline;
    pipeline.SetFileSystemMonitoring(false);
    pipeline.SetProcessingThreadCount(1);
    EXPECT_TRUE(pipeline.Initialize());

    const fs::path unknownPath = assets.Write("unsupported.importer");
    std::atomic<int> callbackCount{0};
    std::atomic<bool> callbackFailed{false};
    EXPECT_FALSE(pipeline.ProcessAsset(unknownPath.string(), PassThroughSettings(),
                                       [&](const SparkEditor::AssetMetadata& metadata)
                                       {
                                           callbackFailed.store(metadata.status ==
                                                                    SparkEditor::ProcessingStatus::FAILED &&
                                                                !metadata.errorMessage.empty());
                                           ++callbackCount;
                                       }));
    EXPECT_EQ(callbackCount.load(), 1);
    EXPECT_TRUE(callbackFailed.load());

    std::atomic<int> batchCompletionCount{0};
    const uint32_t rejectedBatchID = pipeline.ProcessAssetsBatch({unknownPath.string()}, PassThroughSettings(), nullptr,
                                                                 [&] { ++batchCompletionCount; });
    SparkEditor::BatchOperation rejectedBatch;
    EXPECT_TRUE(pipeline.GetBatchOperation(rejectedBatchID, rejectedBatch));
    EXPECT_FALSE(rejectedBatch.isActive);
    EXPECT_TRUE(rejectedBatch.status == SparkEditor::ProcessingStatus::FAILED);
    EXPECT_EQ(rejectedBatch.completedAssets, 1);
    EXPECT_EQ(batchCompletionCount.load(), 0);
    pipeline.Update(0.0f);
    EXPECT_EQ(batchCompletionCount.load(), 1);
}

TEST(AdvancedAssetPipeline_RepeatedInitializeShutdownDoesNotHang)
{
    // Regression: Shutdown() and SetProcessingThreadCount() used to publish the
    // worker stop flag without holding the queue mutex, so a worker between its
    // wait-predicate check and blocking missed the notification and join()
    // hung (build-linux-asan timed out on 2026-09-02). Cycling start-up and
    // shutdown quickly keeps re-opening that window.
    for (int cycle = 0; cycle < 256; ++cycle)
    {
        SparkEditor::AdvancedAssetPipeline pipeline;
        pipeline.SetFileSystemMonitoring(false);
        pipeline.SetProcessingThreadCount(2);
        EXPECT_TRUE(pipeline.Initialize());
        pipeline.SetProcessingThreadCount(1);
        pipeline.Shutdown();
    }
}
