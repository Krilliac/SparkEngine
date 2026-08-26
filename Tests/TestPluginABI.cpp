#include "TestFramework.h"
#include "Core/DynamicPluginHost.h"

#include <Spark/PluginABI.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
    SparkPluginResult CreatePlugin(const SparkPluginHostAPI*, SparkPluginInstance* outInstance)
    {
        if (!outInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        *outInstance = 42;
        return SPARK_PLUGIN_OK;
    }

    void DestroyPlugin(SparkPluginInstance) {}

    SparkPluginAPI MakeAPI()
    {
        SparkPluginAPI api{};
        api.struct_size = sizeof(api);
        api.abi_major = SPARK_PLUGIN_ABI_MAJOR;
        api.abi_minor = SPARK_PLUGIN_ABI_MINOR;
        api.create = &CreatePlugin;
        api.destroy = &DestroyPlugin;
        return api;
    }

    SparkPluginDescriptor MakeDescriptor(const SparkPluginAPI* api)
    {
        SparkPluginDescriptor descriptor{};
        descriptor.struct_size = sizeof(descriptor);
        descriptor.magic = SPARK_PLUGIN_ABI_MAGIC;
        descriptor.abi_major = SPARK_PLUGIN_ABI_MAJOR;
        descriptor.abi_minor = SPARK_PLUGIN_ABI_MINOR;
        descriptor.id = "org.sparkengine.test";
        descriptor.name = "Spark ABI Test";
        descriptor.api = api;
        return descriptor;
    }

    const SparkPluginHostAPI* g_host = nullptr;
    bool g_started = false;
    bool g_stopped = false;
    bool g_destroyed = false;
    int g_ticks = 0;
    std::atomic<bool> g_prepareEntered{false};
    std::atomic<bool> g_releasePrepare{false};
    SparkPluginResult g_stopScheduleResult = SPARK_PLUGIN_ERROR_INTERNAL;

    constexpr SparkPluginInstance kReloadOldInstance = 101;
    constexpr SparkPluginInstance kReloadNewInstance = 202;
    uint32_t g_reloadOldValue = 0;
    uint32_t g_reloadNewValue = 0;
    bool g_reloadFailRestore = false;
    bool g_reloadFailStartNew = false;
    bool g_reloadOversizeSave = false;
    bool g_reloadOldStarted = false;
    bool g_reloadNewStarted = false;
    bool g_reloadOldStopped = false;
    bool g_reloadNewStopped = false;
    bool g_reloadOldDestroyed = false;
    bool g_reloadNewDestroyed = false;
    bool g_reloadOldActive = false;
    bool g_reloadNewActive = false;
    bool g_reloadGenerationOverlap = false;
    int g_reloadActiveCount = 0;
    int g_reloadCancelCount = 0;
    const SparkPluginHostAPI* g_reloadOldHost = nullptr;
    const SparkPluginHostAPI* g_reloadNewHost = nullptr;

    SparkPluginResult HostCreate(const SparkPluginHostAPI* host, SparkPluginInstance* outInstance)
    {
        if (!host || !outInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        g_host = host;
        *outInstance = 77;
        return SPARK_PLUGIN_OK;
    }

    void HostDestroy(SparkPluginInstance instance)
    {
        g_destroyed = instance == 77;
    }

    SparkPluginResult HostStart(SparkPluginInstance instance)
    {
        g_started = instance == 77;
        return g_started ? SPARK_PLUGIN_OK : SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    void HostStop(SparkPluginInstance instance)
    {
        g_stopped = instance == 77;
    }

    SparkPluginResult HostTick(SparkPluginInstance instance, double)
    {
        if (instance != 77)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        ++g_ticks;
        return SPARK_PLUGIN_OK;
    }

    SparkPluginResult HostPrepareUnload(SparkPluginInstance instance, uint32_t)
    {
        return instance == 77 ? SPARK_PLUGIN_OK : SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    void IncrementTask(void* context);

    SparkPluginResult BlockingPrepareUnload(SparkPluginInstance instance, uint32_t)
    {
        if (instance != 77)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        g_prepareEntered.store(true, std::memory_order_release);
        while (!g_releasePrepare.load(std::memory_order_acquire))
            std::this_thread::yield();
        return SPARK_PLUGIN_OK;
    }

    void StopAttemptsLateSchedule(SparkPluginInstance instance)
    {
        g_stopped = instance == 77;
        SparkPluginTask ignored = 0;
        g_stopScheduleResult = g_host->schedule_task(g_host->host_context, &IncrementTask, nullptr, &ignored);
    }

    void IncrementTask(void* context)
    {
        static_cast<std::atomic<int>*>(context)->fetch_add(1, std::memory_order_relaxed);
    }

    struct BlockingTaskContext
    {
        std::atomic<bool> started{false};
        std::atomic<bool> release{false};
    };

    void BlockingTask(void* context)
    {
        auto& state = *static_cast<BlockingTaskContext*>(context);
        state.started.store(true, std::memory_order_release);
        while (!state.release.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    struct SelfWaitContext
    {
        std::atomic<SparkPluginTask> id{0};
        std::atomic<SparkPluginResult> result{SPARK_PLUGIN_ERROR_INTERNAL};
    };

    void WaitOnSelf(void* context)
    {
        auto& state = *static_cast<SelfWaitContext*>(context);
        SparkPluginTask id = 0;
        while ((id = state.id.load(std::memory_order_acquire)) == 0)
            std::this_thread::yield();
        state.result.store(g_host->wait_task(g_host->host_context, id, UINT32_MAX), std::memory_order_release);
    }

    SparkPluginAPI MakeHostAPI()
    {
        SparkPluginAPI api = MakeAPI();
        api.capabilities = SPARK_PLUGIN_CAP_TICK;
        api.create = &HostCreate;
        api.destroy = &HostDestroy;
        api.start = &HostStart;
        api.stop = &HostStop;
        api.tick = &HostTick;
        api.prepare_unload = &HostPrepareUnload;
        return api;
    }

    void ResetReloadState()
    {
        g_reloadOldValue = 0x5a17c0deu;
        g_reloadNewValue = 0;
        g_reloadFailRestore = false;
        g_reloadFailStartNew = false;
        g_reloadOversizeSave = false;
        g_reloadOldStarted = false;
        g_reloadNewStarted = false;
        g_reloadOldStopped = false;
        g_reloadNewStopped = false;
        g_reloadOldDestroyed = false;
        g_reloadNewDestroyed = false;
        g_reloadOldActive = false;
        g_reloadNewActive = false;
        g_reloadGenerationOverlap = false;
        g_reloadActiveCount = 0;
        g_reloadCancelCount = 0;
        g_reloadOldHost = nullptr;
        g_reloadNewHost = nullptr;
    }

    SparkPluginResult CreateReloadOld(const SparkPluginHostAPI* host, SparkPluginInstance* outInstance)
    {
        if (!host || !outInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        g_reloadOldHost = host;
        *outInstance = kReloadOldInstance;
        return SPARK_PLUGIN_OK;
    }

    SparkPluginResult CreateReloadNew(const SparkPluginHostAPI* host, SparkPluginInstance* outInstance)
    {
        if (!host || !outInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        g_reloadNewHost = host;
        *outInstance = kReloadNewInstance;
        return SPARK_PLUGIN_OK;
    }

    void DestroyReloadPlugin(SparkPluginInstance instance)
    {
        if (instance == kReloadOldInstance)
            g_reloadOldDestroyed = true;
        if (instance == kReloadNewInstance)
            g_reloadNewDestroyed = true;
    }

    SparkPluginResult StartReloadPlugin(SparkPluginInstance instance)
    {
        if (instance == kReloadOldInstance)
        {
            g_reloadOldStarted = true;
            g_reloadOldActive = true;
        }
        else if (instance == kReloadNewInstance)
        {
            if (g_reloadFailStartNew)
                return SPARK_PLUGIN_ERROR_INTERNAL;
            g_reloadNewStarted = true;
            g_reloadNewActive = true;
        }
        else
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        g_reloadGenerationOverlap = g_reloadGenerationOverlap || g_reloadActiveCount != 0;
        ++g_reloadActiveCount;
        return SPARK_PLUGIN_OK;
    }

    void StopReloadPlugin(SparkPluginInstance instance)
    {
        if (instance == kReloadOldInstance)
        {
            g_reloadOldStopped = true;
            if (g_reloadOldActive)
            {
                g_reloadOldActive = false;
                --g_reloadActiveCount;
            }
        }
        if (instance == kReloadNewInstance)
        {
            g_reloadNewStopped = true;
            if (g_reloadNewActive)
            {
                g_reloadNewActive = false;
                --g_reloadActiveCount;
            }
        }
    }

    SparkPluginResult TickReloadPlugin(SparkPluginInstance instance, double)
    {
        return instance == kReloadOldInstance || instance == kReloadNewInstance ? SPARK_PLUGIN_OK
                                                                                : SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    SparkPluginResult PrepareReloadPlugin(SparkPluginInstance instance, uint32_t)
    {
        return instance == kReloadOldInstance || instance == kReloadNewInstance ? SPARK_PLUGIN_OK
                                                                                : SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
    }

    SparkPluginResult CancelReloadPlugin(SparkPluginInstance instance)
    {
        if (instance != kReloadOldInstance && instance != kReloadNewInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        ++g_reloadCancelCount;
        return SPARK_PLUGIN_OK;
    }

    SparkPluginResult SaveReloadState(SparkPluginInstance instance, SparkPluginMutableBytes* state)
    {
        if (!state || !state->data || (instance != kReloadOldInstance && instance != kReloadNewInstance))
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        if (g_reloadOversizeSave && instance == kReloadOldInstance)
        {
            state->size = state->capacity + 1;
            return SPARK_PLUGIN_OK;
        }
        if (state->capacity < sizeof(uint32_t))
            return SPARK_PLUGIN_ERROR_OUT_OF_MEMORY;
        const uint32_t value = instance == kReloadOldInstance ? g_reloadOldValue : g_reloadNewValue;
        std::memcpy(state->data, &value, sizeof(value));
        state->size = sizeof(value);
        return SPARK_PLUGIN_OK;
    }

    SparkPluginResult RestoreReloadState(SparkPluginInstance instance, SparkPluginBytes state)
    {
        if (instance != kReloadOldInstance && instance != kReloadNewInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        if (instance == kReloadNewInstance && g_reloadFailRestore)
            return SPARK_PLUGIN_ERROR_INTERNAL;
        if (!state.data || state.size != sizeof(uint32_t))
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        uint32_t value = 0;
        std::memcpy(&value, state.data, sizeof(value));
        if (instance == kReloadOldInstance)
            g_reloadOldValue = value;
        else
            g_reloadNewValue = value;
        return SPARK_PLUGIN_OK;
    }

    SparkPluginAPI MakeReloadAPI(bool replacement)
    {
        SparkPluginAPI api = MakeAPI();
        api.capabilities = SPARK_PLUGIN_CAP_TICK | SPARK_PLUGIN_CAP_HOT_RELOAD;
        api.create = replacement ? &CreateReloadNew : &CreateReloadOld;
        api.destroy = &DestroyReloadPlugin;
        api.start = &StartReloadPlugin;
        api.stop = &StopReloadPlugin;
        api.tick = &TickReloadPlugin;
        api.prepare_unload = &PrepareReloadPlugin;
        api.save_state = &SaveReloadState;
        api.restore_state = &RestoreReloadState;
        api.cancel_unload = &CancelReloadPlugin;
        return api;
    }

    class TemporaryPluginBinary
    {
      public:
        TemporaryPluginBinary()
        {
            const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("spark-plugin-integrity-" + std::to_string(nonce) +
#if defined(_WIN32)
                                                             ".dll");
#else
                                                             ".so");
#endif
            metadataPath = path;
            metadataPath += ".sparkplugin.json";
            std::ofstream binary(path, std::ios::binary | std::ios::trunc);
            binary << "abc";
        }

        ~TemporaryPluginBinary()
        {
            std::error_code ignored;
            std::filesystem::remove(metadataPath, ignored);
            std::filesystem::remove(path, ignored);
        }

        void WriteMetadata(const std::string& digest, bool includeUnexpectedField = false) const
        {
            std::ofstream metadata(metadataPath, std::ios::binary | std::ios::trunc);
            metadata << "{\n"
                     << "  \"schema\": 1,\n"
                     << "  \"id\": \"org.sparkengine.integrity-test\",\n"
                     << "  \"version\": \"1.0.0\",\n"
                     << "  \"type\": \"runtime-extension\",\n"
                     << "  \"abi_major\": " << SPARK_PLUGIN_ABI_MAJOR << ",\n"
                     << "  \"abi_minor\": " << SPARK_PLUGIN_ABI_MINOR << ",\n"
                     << "  \"entry_point\": \"" SPARK_PLUGIN_ENTRY_POINT "\",\n"
                     << "  \"binary\": \"" << path.filename().string() << "\",\n"
                     << "  \"sha256\": \"" << digest << "\"";
            if (includeUnexpectedField)
                metadata << ",\n  \"unexpected\": true";
            metadata << "\n}\n";
        }

        std::filesystem::path path;
        std::filesystem::path metadataPath;
    };
} // namespace

TEST(PluginABI_AcceptsCompatibleAppendOnlyTables)
{
    const SparkPluginAPI api = MakeAPI();
    const SparkPluginDescriptor descriptor = MakeDescriptor(&api);
    EXPECT_EQ(SparkValidatePluginDescriptor(&descriptor), SPARK_PLUGIN_OK);
}

TEST(PluginABI_RejectsMajorOrForwardMinorMismatch)
{
    SparkPluginAPI api = MakeAPI();
    SparkPluginDescriptor descriptor = MakeDescriptor(&api);

    ++descriptor.abi_major;
    EXPECT_EQ(SparkValidatePluginDescriptor(&descriptor), SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI);
    descriptor.abi_major = SPARK_PLUGIN_ABI_MAJOR;
    descriptor.abi_minor = SPARK_PLUGIN_ABI_MINOR + 1;
    EXPECT_EQ(SparkValidatePluginDescriptor(&descriptor), SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI);
}

TEST(PluginABI_RequiresFactoriesAndMinimumTableSizes)
{
    SparkPluginAPI api = MakeAPI();
    SparkPluginDescriptor descriptor = MakeDescriptor(&api);

    api.destroy = nullptr;
    EXPECT_EQ(SparkValidatePluginDescriptor(&descriptor), SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI);
    api.destroy = &DestroyPlugin;
    api.struct_size = static_cast<uint32_t>(offsetof(SparkPluginAPI, restore_state));
    EXPECT_EQ(SparkValidatePluginDescriptor(&descriptor), SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI);
}

TEST(PluginABI_HotReloadCapabilityRequiresCompleteStateContract)
{
    SparkPluginAPI api = MakeAPI();
    SparkPluginDescriptor descriptor = MakeDescriptor(&api);
    api.capabilities = SPARK_PLUGIN_CAP_HOT_RELOAD;
    EXPECT_EQ(SparkValidatePluginDescriptor(&descriptor), SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI);

    api.prepare_unload = &PrepareReloadPlugin;
    api.save_state = &SaveReloadState;
    api.restore_state = &RestoreReloadState;
    api.cancel_unload = &CancelReloadPlugin;
    EXPECT_EQ(SparkValidatePluginDescriptor(&descriptor), SPARK_PLUGIN_OK);

    api.cancel_unload = nullptr;
    EXPECT_EQ(SparkValidatePluginDescriptor(&descriptor), SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI);
}

TEST(DynamicPluginHost_OwnsLifecycleAndTaskFence)
{
    g_host = nullptr;
    g_started = false;
    g_stopped = false;
    g_destroyed = false;
    g_ticks = 0;

    const SparkPluginAPI api = MakeHostAPI();
    const SparkPluginDescriptor descriptor = MakeDescriptor(&api);
    Spark::DynamicPluginHost host;
    std::string error;
    EXPECT_TRUE(host.AttachDescriptorForTesting(&descriptor, &error));
    EXPECT_EQ(static_cast<int>(host.GetState()), static_cast<int>(Spark::DynamicPluginHost::State::Loaded));
    EXPECT_TRUE(host.Start(&error));
    EXPECT_TRUE(g_started);
    EXPECT_EQ(host.Tick(1.0 / 60.0), SPARK_PLUGIN_OK);
    EXPECT_EQ(g_ticks, 1);

    std::atomic<int> completed{0};
    SparkPluginTask task = 0;
    EXPECT_TRUE(g_host != nullptr);
    EXPECT_EQ(g_host->schedule_task(g_host->host_context, &IncrementTask, &completed, &task), SPARK_PLUGIN_OK);
    EXPECT_NE(task, SparkPluginTask{0});
    EXPECT_EQ(g_host->wait_task(g_host->host_context, task, 5000), SPARK_PLUGIN_OK);
    EXPECT_EQ(completed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(host.ActiveTaskCount(), size_t{0});

    EXPECT_TRUE(host.Unload(std::chrono::seconds(1), &error));
    EXPECT_TRUE(g_stopped);
    EXPECT_TRUE(g_destroyed);
    EXPECT_FALSE(host.IsLoaded());

    EXPECT_TRUE(host.AttachDescriptorForTesting(&descriptor, &error));
    EXPECT_TRUE(host.Unload(std::chrono::seconds(1), &error));
}

TEST(DynamicPluginHost_ProvidesAlignedHostOwnedMemoryAndResources)
{
    const SparkPluginAPI api = MakeHostAPI();
    const SparkPluginDescriptor descriptor = MakeDescriptor(&api);
    Spark::DynamicPluginHost host;
    host.SetResourceResolver(
        [](const char* id, SparkPluginResource* out)
        {
            if (std::strcmp(id, "textures/checker") != 0)
                return SPARK_PLUGIN_ERROR_NOT_READY;
            *out = 991;
            return SPARK_PLUGIN_OK;
        });
    EXPECT_TRUE(host.AttachDescriptorForTesting(&descriptor));

    auto* bytes = static_cast<unsigned char*>(g_host->allocate(g_host->host_context, 32, 64, 0));
    EXPECT_TRUE(bytes != nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(bytes) % 64, uintptr_t{0});
    std::memset(bytes, 0x5a, 32);
    bytes = static_cast<unsigned char*>(g_host->reallocate(g_host->host_context, bytes, 96, 64, 0));
    EXPECT_TRUE(bytes != nullptr);
    EXPECT_EQ(bytes[31], static_cast<unsigned char>(0x5a));
    g_host->deallocate(g_host->host_context, bytes, 64, 0);

    SparkPluginResource resource = 0;
    EXPECT_EQ(g_host->resolve_resource(g_host->host_context, "textures/checker", &resource), SPARK_PLUGIN_OK);
    EXPECT_EQ(resource, SparkPluginResource{991});
    EXPECT_TRUE(host.Unload());
}

TEST(DynamicPluginHost_ContainsThrowingHostCallbacksAndRejectsTaskSelfWait)
{
    g_host = nullptr;
    const SparkPluginAPI api = MakeHostAPI();
    const SparkPluginDescriptor descriptor = MakeDescriptor(&api);
    Spark::DynamicPluginHost host;
    host.SetLogSink([](SparkPluginLogLevel, const char*, const char*) { throw std::runtime_error("log"); });
    host.SetResourceResolver([](const char*, SparkPluginResource*) -> SparkPluginResult
                             { throw std::runtime_error("resolve"); });
    EXPECT_TRUE(host.AttachDescriptorForTesting(&descriptor));

    g_host->log(g_host->host_context, SPARK_PLUGIN_LOG_INFO, "test", "contained");
    SparkPluginResource resource = 99;
    EXPECT_EQ(g_host->resolve_resource(g_host->host_context, "throw", &resource), SPARK_PLUGIN_ERROR_INTERNAL);
    EXPECT_EQ(resource, SparkPluginResource{0});

    SelfWaitContext selfWait;
    SparkPluginTask task = 0;
    EXPECT_EQ(g_host->schedule_task(g_host->host_context, &WaitOnSelf, &selfWait, &task), SPARK_PLUGIN_OK);
    selfWait.id.store(task, std::memory_order_release);
    EXPECT_EQ(g_host->wait_task(g_host->host_context, task, 5000), SPARK_PLUGIN_OK);
    EXPECT_EQ(selfWait.result.load(std::memory_order_acquire), SPARK_PLUGIN_ERROR_BUSY);
    EXPECT_TRUE(host.Unload());
}

TEST(DynamicPluginHost_ClosesSchedulingGateBeforeUnloadFences)
{
    g_host = nullptr;
    g_stopped = false;
    g_destroyed = false;
    g_prepareEntered.store(false, std::memory_order_relaxed);
    g_releasePrepare.store(false, std::memory_order_relaxed);
    g_stopScheduleResult = SPARK_PLUGIN_ERROR_INTERNAL;

    SparkPluginAPI api = MakeHostAPI();
    api.prepare_unload = &BlockingPrepareUnload;
    api.stop = &StopAttemptsLateSchedule;
    const SparkPluginDescriptor descriptor = MakeDescriptor(&api);
    Spark::DynamicPluginHost host;
    std::string error;
    EXPECT_TRUE(host.AttachDescriptorForTesting(&descriptor, &error));
    EXPECT_TRUE(host.Start(&error));

    bool unloaded = false;
    std::thread unloadThread([&] { unloaded = host.Unload(std::chrono::seconds(2), &error); });
    while (!g_prepareEntered.load(std::memory_order_acquire))
        std::this_thread::yield();

    SparkPluginTask lateTask = 0;
    std::atomic<int> lateTaskRuns{0};
    EXPECT_EQ(g_host->schedule_task(g_host->host_context, &IncrementTask, &lateTaskRuns, &lateTask),
              SPARK_PLUGIN_ERROR_BUSY);
    g_releasePrepare.store(true, std::memory_order_release);
    unloadThread.join();

    EXPECT_TRUE(unloaded);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(g_stopped);
    EXPECT_TRUE(g_destroyed);
    EXPECT_EQ(g_stopScheduleResult, SPARK_PLUGIN_ERROR_BUSY);
    EXPECT_EQ(lateTaskRuns.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(host.ActiveTaskCount(), size_t{0});
}

TEST(DynamicPluginHost_TransactionalReloadRestoresStateBeforeCommit)
{
    ResetReloadState();
    SparkPluginAPI oldApi = MakeReloadAPI(false);
    SparkPluginAPI newApi = MakeReloadAPI(true);
    SparkPluginDescriptor oldDescriptor = MakeDescriptor(&oldApi);
    SparkPluginDescriptor newDescriptor = MakeDescriptor(&newApi);

    Spark::DynamicPluginHost host;
    std::string error;
    EXPECT_TRUE(host.AttachDescriptorForTesting(&oldDescriptor, &error));
    EXPECT_TRUE(host.Start(&error));
    EXPECT_TRUE(host.ReloadDescriptorForTesting(&newDescriptor, std::chrono::seconds(1), &error));

    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(host.Descriptor() == &newDescriptor);
    EXPECT_EQ(g_reloadNewValue, g_reloadOldValue);
    EXPECT_TRUE(g_reloadOldStopped);
    EXPECT_TRUE(g_reloadOldDestroyed);
    EXPECT_TRUE(g_reloadNewStarted);
    EXPECT_FALSE(g_reloadGenerationOverlap);
    EXPECT_EQ(host.Tick(0.016), SPARK_PLUGIN_OK);

    EXPECT_TRUE(host.Unload(std::chrono::seconds(1), &error));
    EXPECT_TRUE(g_reloadNewStopped);
    EXPECT_TRUE(g_reloadNewDestroyed);
}

TEST(DynamicPluginHost_RestoreFailureRetainsUsableOldPlugin)
{
    ResetReloadState();
    g_reloadFailRestore = true;
    SparkPluginAPI oldApi = MakeReloadAPI(false);
    SparkPluginAPI newApi = MakeReloadAPI(true);
    SparkPluginDescriptor oldDescriptor = MakeDescriptor(&oldApi);
    SparkPluginDescriptor newDescriptor = MakeDescriptor(&newApi);

    Spark::DynamicPluginHost host;
    std::string error;
    EXPECT_TRUE(host.AttachDescriptorForTesting(&oldDescriptor, &error));
    EXPECT_TRUE(host.Start(&error));
    EXPECT_FALSE(host.ReloadDescriptorForTesting(&newDescriptor, std::chrono::seconds(1), &error));

    EXPECT_TRUE(error.find("restore_state") != std::string::npos);
    EXPECT_TRUE(host.Descriptor() == &oldDescriptor);
    EXPECT_FALSE(g_reloadOldStopped);
    EXPECT_FALSE(g_reloadOldDestroyed);
    EXPECT_TRUE(g_reloadNewDestroyed);
    EXPECT_EQ(g_reloadCancelCount, 1);
    EXPECT_EQ(host.Tick(0.016), SPARK_PLUGIN_OK);

    g_reloadFailRestore = false;
    EXPECT_TRUE(host.Unload(std::chrono::seconds(1)));
}

TEST(DynamicPluginHost_TaskFenceTimeoutCancelsPreparedReloadAndReopensScheduling)
{
    ResetReloadState();
    SparkPluginAPI oldApi = MakeReloadAPI(false);
    SparkPluginAPI newApi = MakeReloadAPI(true);
    SparkPluginDescriptor oldDescriptor = MakeDescriptor(&oldApi);
    SparkPluginDescriptor newDescriptor = MakeDescriptor(&newApi);

    Spark::DynamicPluginHost host;
    std::string error;
    EXPECT_TRUE(host.AttachDescriptorForTesting(&oldDescriptor, &error));
    EXPECT_TRUE(host.Start(&error));
    EXPECT_TRUE(g_reloadOldHost != nullptr);

    BlockingTaskContext blocking;
    SparkPluginTask blockingId = 0;
    EXPECT_EQ(g_reloadOldHost->schedule_task(g_reloadOldHost->host_context, &BlockingTask, &blocking, &blockingId),
              SPARK_PLUGIN_OK);
    while (!blocking.started.load(std::memory_order_acquire))
        std::this_thread::yield();

    EXPECT_FALSE(host.ReloadDescriptorForTesting(&newDescriptor, std::chrono::milliseconds(1), &error));
    EXPECT_TRUE(error.find("task fence timed out") != std::string::npos);
    EXPECT_EQ(g_reloadCancelCount, 1);
    EXPECT_EQ(host.Tick(0.016), SPARK_PLUGIN_OK);

    std::atomic<int> followupRuns{0};
    SparkPluginTask followupId = 0;
    EXPECT_EQ(g_reloadOldHost->schedule_task(g_reloadOldHost->host_context, &IncrementTask, &followupRuns, &followupId),
              SPARK_PLUGIN_OK);
    blocking.release.store(true, std::memory_order_release);
    EXPECT_EQ(g_reloadOldHost->wait_task(g_reloadOldHost->host_context, blockingId, 5000), SPARK_PLUGIN_OK);
    EXPECT_EQ(g_reloadOldHost->wait_task(g_reloadOldHost->host_context, followupId, 5000), SPARK_PLUGIN_OK);
    EXPECT_EQ(followupRuns.load(std::memory_order_relaxed), 1);

    EXPECT_TRUE(host.Unload(std::chrono::seconds(1), &error));
}

TEST(DynamicPluginHost_ReplacementValidationFailureLeavesOldPluginUntouched)
{
    ResetReloadState();
    SparkPluginAPI oldApi = MakeReloadAPI(false);
    SparkPluginAPI invalidNewApi = MakeReloadAPI(true);
    invalidNewApi.create = nullptr;
    SparkPluginDescriptor oldDescriptor = MakeDescriptor(&oldApi);
    SparkPluginDescriptor invalidNewDescriptor = MakeDescriptor(&invalidNewApi);

    Spark::DynamicPluginHost host;
    std::string error;
    EXPECT_TRUE(host.AttachDescriptorForTesting(&oldDescriptor, &error));
    EXPECT_TRUE(host.Start(&error));
    EXPECT_FALSE(host.ReloadDescriptorForTesting(&invalidNewDescriptor, std::chrono::seconds(1), &error));

    EXPECT_TRUE(error.find("replacement descriptor failed") != std::string::npos);
    EXPECT_TRUE(host.Descriptor() == &oldDescriptor);
    EXPECT_FALSE(g_reloadOldStopped);
    EXPECT_FALSE(g_reloadOldDestroyed);
    EXPECT_FALSE(g_reloadNewDestroyed);
    EXPECT_EQ(host.Tick(0.016), SPARK_PLUGIN_OK);

    EXPECT_TRUE(host.Unload(std::chrono::seconds(1)));
}

TEST(DynamicPluginHost_RejectsOversizedReloadStateAndRetainsOldPlugin)
{
    ResetReloadState();
    g_reloadOversizeSave = true;
    SparkPluginAPI oldApi = MakeReloadAPI(false);
    SparkPluginAPI newApi = MakeReloadAPI(true);
    SparkPluginDescriptor oldDescriptor = MakeDescriptor(&oldApi);
    SparkPluginDescriptor newDescriptor = MakeDescriptor(&newApi);

    Spark::DynamicPluginHost host;
    std::string error;
    EXPECT_TRUE(host.AttachDescriptorForTesting(&oldDescriptor, &error));
    EXPECT_TRUE(host.Start(&error));
    EXPECT_FALSE(host.ReloadDescriptorForTesting(&newDescriptor, std::chrono::seconds(1), &error));

    EXPECT_TRUE(error.find("bounded buffer") != std::string::npos);
    EXPECT_TRUE(host.Descriptor() == &oldDescriptor);
    EXPECT_FALSE(g_reloadOldDestroyed);
    EXPECT_TRUE(g_reloadNewDestroyed);
    EXPECT_EQ(g_reloadCancelCount, 1);
    EXPECT_EQ(host.Tick(0.016), SPARK_PLUGIN_OK);

    g_reloadOversizeSave = false;
    EXPECT_TRUE(host.Unload(std::chrono::seconds(1)));
}

TEST(DynamicPluginHost_StartFailureRestartsOldGenerationWithoutOverlap)
{
    ResetReloadState();
    g_reloadFailStartNew = true;
    SparkPluginAPI oldApi = MakeReloadAPI(false);
    SparkPluginAPI newApi = MakeReloadAPI(true);
    SparkPluginDescriptor oldDescriptor = MakeDescriptor(&oldApi);
    SparkPluginDescriptor newDescriptor = MakeDescriptor(&newApi);

    Spark::DynamicPluginHost host;
    std::string error;
    EXPECT_TRUE(host.AttachDescriptorForTesting(&oldDescriptor, &error));
    EXPECT_TRUE(host.Start(&error));
    EXPECT_FALSE(host.ReloadDescriptorForTesting(&newDescriptor, std::chrono::seconds(1), &error));

    EXPECT_TRUE(error.find("replacement start failed") != std::string::npos);
    EXPECT_TRUE(host.Descriptor() == &oldDescriptor);
    EXPECT_TRUE(g_reloadOldStopped);
    EXPECT_TRUE(g_reloadOldActive);
    EXPECT_FALSE(g_reloadGenerationOverlap);
    EXPECT_EQ(g_reloadCancelCount, 1);
    EXPECT_EQ(host.Tick(0.016), SPARK_PLUGIN_OK);

    g_reloadFailStartNew = false;
    EXPECT_TRUE(host.Unload(std::chrono::seconds(1)));
}

TEST(DynamicPluginHost_ValidatesMetadataAndSha256BeforeMapping)
{
    TemporaryPluginBinary plugin;
    Spark::DynamicPluginHost host;
    std::string error;

    EXPECT_FALSE(host.Load(plugin.path, &error));
    EXPECT_TRUE(error.find("missing mandatory plugin metadata") != std::string::npos);

    plugin.WriteMetadata(std::string(64, '0'));
    error.clear();
    EXPECT_FALSE(host.Load(plugin.path, &error));
    EXPECT_TRUE(error.find("binary SHA-256 mismatch") != std::string::npos);

    plugin.WriteMetadata("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", true);
    error.clear();
    EXPECT_FALSE(host.Load(plugin.path, &error));
    EXPECT_TRUE(error.find("exactly the schema 1 fields") != std::string::npos);

    // The published SHA-256 of "abc" passes metadata validation. Only then may
    // the platform loader reject this deliberately non-native test image.
    plugin.WriteMetadata("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    error.clear();
    EXPECT_FALSE(host.Load(plugin.path, &error));
#if defined(_WIN32)
    EXPECT_TRUE(error.find("LoadLibraryExW failed") != std::string::npos);
#else
    EXPECT_TRUE(error.find("dlopen") != std::string::npos || error.find("file too short") != std::string::npos);
#endif
}
