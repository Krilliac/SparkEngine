/**
 * @file TelemetrySpoolInternal.h
 * @brief Shared declarations for the TelemetrySpool translation units.
 *
 * This is a private implementation header -- not part of the public API.
 * It exposes format constants and helper functions that the three
 * TelemetrySpool*.cpp files need to share.
 */

#pragma once

#include "Utils/TelemetrySpool.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Spark
{
    struct TelemetryEvent;
}

namespace Spark::TelemetryDetail
{
    // =========================================================================
    // Format constants (defined inline -- used by Format, IO, and main TU)
    // =========================================================================

    inline constexpr size_t kMagicSize = 8;
    inline constexpr size_t kHeaderBytes = kMagicSize + sizeof(uint32_t) + sizeof(uint32_t);
    inline constexpr uint32_t kAbsoluteMaxEvents = 100000;

    // =========================================================================
    // Serialization helpers (defined in TelemetrySpoolFormat.cpp)
    // =========================================================================

    bool TryResize(std::vector<uint8_t>& bytes, size_t size);

    bool EventSerializedSize(const TelemetryEvent& event, size_t& size);

    bool Serialize(const std::vector<TelemetryEvent>& events, uint64_t maximumBytes, std::vector<uint8_t>& bytes);

    bool Parse(const std::vector<uint8_t>& bytes, uint32_t maximumEvents, std::vector<TelemetryEvent>& events);

    // =========================================================================
    // Filesystem safety / platform I/O (defined in TelemetrySpoolIO.cpp)
    // =========================================================================

    bool PathExists(const std::filesystem::path& path, bool& exists);

    bool IsSafeDirectoryPath(const std::filesystem::path& path);

    TelemetrySpoolResult RemoveOwnedFile(const std::filesystem::path& path);

    TelemetrySpoolResult ReadOwnedFile(const std::filesystem::path& path, uint64_t maximumBytes,
                                       std::vector<uint8_t>& bytes);

    TelemetrySpoolResult WriteOwnedFileAtomically(const std::filesystem::path& directory,
                                                  const std::filesystem::path& artifact,
                                                  const std::filesystem::path& staging,
                                                  const std::vector<uint8_t>& bytes);

} // namespace Spark::TelemetryDetail
