#pragma once

/**
 * @file DynamicPluginHost.h
 * @brief Production loader and lifecycle owner for the stable Spark plugin C ABI.
 */

#include <Spark/PluginABI.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace Spark
{
    class DynamicPluginHost
    {
      public:
        using LogSink = std::function<void(SparkPluginLogLevel, const char*, const char*)>;
        using ResourceResolver = std::function<SparkPluginResult(const char*, SparkPluginResource*)>;

        enum class State : uint8_t
        {
            Empty,
            Loaded,
            Started,
            Stopped,
        };

        DynamicPluginHost();
        ~DynamicPluginHost();

        DynamicPluginHost(const DynamicPluginHost&) = delete;
        DynamicPluginHost& operator=(const DynamicPluginHost&) = delete;
        DynamicPluginHost(DynamicPluginHost&&) = delete;
        DynamicPluginHost& operator=(DynamicPluginHost&&) = delete;

        void SetLogSink(LogSink sink);
        void SetResourceResolver(ResourceResolver resolver);

        /**
         * Load, validate, and instantiate one plugin from an absolute native-library path.
         * The sibling `.sparkplugin.json` metadata and binary SHA-256 are mandatory and
         * validated before the native image is mapped.
         */
        bool Load(const std::filesystem::path& path, std::string* error = nullptr);

        /** Start the loaded plugin. A missing optional start callback is a successful no-op. */
        bool Start(std::string* error = nullptr);

        /** Tick a started plugin when it advertises SPARK_PLUGIN_CAP_TICK. */
        SparkPluginResult Tick(double deltaSeconds);

        /**
         * Stop/destroy/unmap after the plugin and every host task reach an unload fence.
         * Failure is fail-closed: the image stays mapped and the instance stays owned.
         */
        bool Unload(std::chrono::milliseconds timeout = std::chrono::seconds(5), std::string* error = nullptr);

        [[nodiscard]] State GetState() const noexcept;
        [[nodiscard]] bool IsLoaded() const noexcept;
        [[nodiscard]] const SparkPluginDescriptor* Descriptor() const noexcept;
        [[nodiscard]] const std::filesystem::path& Path() const noexcept;
        [[nodiscard]] size_t ActiveTaskCount() const;

        /** Test seam: validate and instantiate an in-process descriptor without mapping a library. */
        bool AttachDescriptorForTesting(const SparkPluginDescriptor* descriptor, std::string* error = nullptr);

      private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace Spark
