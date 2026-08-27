#include <Spark/PluginABI.h>

namespace
{
    constexpr SparkPluginInstance kInstance = UINT64_C(0x524F4C4C4241434B);
    unsigned g_prepareUnloadCalls = 0;

    SparkPluginResult Create(const SparkPluginHostAPI* host, SparkPluginInstance* outInstance)
    {
        if (!host || !outInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        g_prepareUnloadCalls = 0;
        *outInstance = kInstance;
        return SPARK_PLUGIN_OK;
    }

    void Destroy(SparkPluginInstance) {}

    SparkPluginResult PrepareUnload(SparkPluginInstance instance, uint32_t)
    {
        if (instance != kInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        ++g_prepareUnloadCalls;
        return g_prepareUnloadCalls == 1 ? SPARK_PLUGIN_ERROR_BUSY : SPARK_PLUGIN_OK;
    }

    const SparkPluginAPI kAPI = {
        sizeof(SparkPluginAPI),
        SPARK_PLUGIN_ABI_MAJOR,
        SPARK_PLUGIN_ABI_MINOR,
        0,
        SPARK_PLUGIN_CAP_EDITOR_EXTENSION,
        &Create,
        &Destroy,
        nullptr,
        nullptr,
        nullptr,
        &PrepareUnload,
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
        "org.sparkengine.test.rollback-refusing-plugin",
        "Spark Rollback Refusing Plugin Fixture",
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
