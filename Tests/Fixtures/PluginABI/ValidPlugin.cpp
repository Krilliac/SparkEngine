#include <Spark/PluginABI.h>

namespace
{
    constexpr SparkPluginInstance kInstance = UINT64_C(0x535041524B);
    const SparkPluginHostAPI* g_host = nullptr;

    void Log(const char* message)
    {
        if (g_host && g_host->log)
            g_host->log(g_host->host_context, SPARK_PLUGIN_LOG_INFO, "PluginFixture", message);
    }

    SparkPluginResult Create(const SparkPluginHostAPI* host, SparkPluginInstance* outInstance)
    {
        if (!host || !outInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        g_host = host;
        *outInstance = kInstance;
        Log("create");
        return SPARK_PLUGIN_OK;
    }

    void Destroy(SparkPluginInstance instance)
    {
        if (instance == kInstance)
            Log("destroy");
        g_host = nullptr;
    }

    SparkPluginResult Start(SparkPluginInstance instance)
    {
        if (instance != kInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        Log("start");
        return SPARK_PLUGIN_OK;
    }

    void Stop(SparkPluginInstance instance)
    {
        if (instance == kInstance)
            Log("stop");
    }

    SparkPluginResult Tick(SparkPluginInstance instance, double deltaSeconds)
    {
        if (instance != kInstance || deltaSeconds < 0.0)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        Log("tick");
        return SPARK_PLUGIN_OK;
    }

    const SparkPluginAPI kAPI = {
        sizeof(SparkPluginAPI),
        SPARK_PLUGIN_ABI_MAJOR,
        SPARK_PLUGIN_ABI_MINOR,
        0,
        SPARK_PLUGIN_CAP_TICK | SPARK_PLUGIN_CAP_EDITOR_EXTENSION,
        &Create,
        &Destroy,
        &Start,
        &Stop,
        &Tick,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        {},
    };

    const SparkPluginDescriptor kDescriptor = {
        sizeof(SparkPluginDescriptor),
        SPARK_PLUGIN_ABI_MAGIC,
        SPARK_PLUGIN_ABI_MAJOR,
        SPARK_PLUGIN_ABI_MINOR,
        1,
        0,
        "org.sparkengine.test.native-plugin",
        "Spark Native Plugin Fixture",
        "SparkEngine Tests",
        "1.0.0",
        &kAPI,
        {},
    };
} // namespace

SPARK_DECLARE_PLUGIN_ENTRY_POINT()
{
    if (host_abi_major != SPARK_PLUGIN_ABI_MAJOR || host_abi_minor < SPARK_PLUGIN_ABI_MINOR)
        return nullptr;
    return &kDescriptor;
}
