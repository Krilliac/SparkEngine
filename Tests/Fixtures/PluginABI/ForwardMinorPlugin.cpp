#include <Spark/PluginABI.h>

namespace
{
    SparkPluginResult Create(const SparkPluginHostAPI* host, SparkPluginInstance* outInstance)
    {
        if (!outInstance)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        if (host && host->log)
            host->log(host->host_context, SPARK_PLUGIN_LOG_ERROR, "PluginFixture", "forward-minor-create");
        *outInstance = 1;
        return SPARK_PLUGIN_OK;
    }

    void Destroy(SparkPluginInstance) {}

    const SparkPluginAPI kAPI = {
        sizeof(SparkPluginAPI),
        SPARK_PLUGIN_ABI_MAJOR,
        SPARK_PLUGIN_ABI_MINOR + 1,
        0,
        SPARK_PLUGIN_CAP_EDITOR_EXTENSION,
        &Create,
        &Destroy,
        nullptr,
        nullptr,
        nullptr,
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
        SPARK_PLUGIN_ABI_MINOR + 1,
        1,
        0,
        "org.sparkengine.test.forward-minor-plugin",
        "Spark Forward-Minor Plugin Fixture",
        "SparkEngine Tests",
        "1.0.0",
        &kAPI,
        {},
    };
} // namespace

SPARK_DECLARE_PLUGIN_ENTRY_POINT()
{
    (void)host_abi_major;
    (void)host_abi_minor;
    return &kDescriptor;
}
