/**
 * @file FuzzCrashManifestProduction.cpp
 * @brief Production adapter for the crash-manifest libFuzzer harness.
 */

#include "FuzzCrashManifestProduction.h"

#include "CrashReporterApp.h"

#include <string_view>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
}

extern "C" int SparkFuzzParseCrashManifest(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || size == 0 || size > kMaxInputBytes)
        return 0;

    SparkCrashReporter::CrashManifest manifest;
    const std::string_view json(reinterpret_cast<const char*>(data), size);
    (void)SparkCrashReporter::ParseManifestJson(json, manifest);
    return 0;
}
