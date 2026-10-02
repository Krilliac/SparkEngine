/**
 * @file ModuleSidecar.h
 * @brief The mandatory .sparkabi sidecar gate run before a game module is mapped
 *
 * Every game module ships `<module>.sparkabi`: twelve `key=value` lines mirroring the
 * SparkModuleCompatibilityDescriptor plus the SHA-256 of the module image. ModuleManager
 * validates it before LoadLibraryExW/dlopen runs any module code, so a module built against
 * another SDK or toolchain, or a sidecar paired with a different image, is refused without
 * executing a constructor.
 *
 * Thread affinity: none; the functions touch only the files they are given.
 * Ownership: no state is kept between calls.
 * Allocation: the sidecar read is capped at 4 KiB; hashing streams the image in 64 KiB blocks.
 */

#pragma once

#include <filesystem>
#include <string>

struct SparkModuleCompatibilityDescriptor;

/**
 * @brief Explain why a module compatibility descriptor is rejected.
 *
 * The stable-v1 module ABI is exact-match only (owner decision OD-02); no
 * N-1 module is loaded or migrated. The diagnostic names the rejected
 * sidecar field, the value this host expects, and the value the module
 * declares, so a rejected module can be matched to the SDK or toolchain it
 * must be rebuilt with. ModuleManager uses it for both the pre-OS-load
 * .sparkabi sidecar gate and the in-image descriptor re-check.
 *
 * @param descriptor Descriptor parsed from the sidecar or returned by the
 *        module's SparkGetModuleCompatibility export; may be null.
 * @return An empty string when the descriptor is compatible, otherwise the
 *         rejection diagnostic.
 */
[[nodiscard]] std::string DescribeModuleCompatibilityRejection(const SparkModuleCompatibilityDescriptor* descriptor);

namespace Spark::ModuleSidecar
{
    /// @brief `<modulePath>.sparkabi`, the sidecar that must accompany @p modulePath.
    [[nodiscard]] std::filesystem::path SidecarPath(const std::filesystem::path& modulePath);

    /**
     * @brief Validate the .sparkabi sidecar of @p modulePath against this host and the image.
     *
     * The sidecar must be at most 4096 bytes of exactly twelve distinct `key=value` lines
     * (key 1..32 bytes, value 1..64 bytes, optional CR before each LF): eleven decimal
     * uint32 descriptor fields that pass DescribeModuleCompatibilityRejection, and a
     * 64-character binary_sha256 equal to the SHA-256 of the module image.
     *
     * @param modulePath Module image whose sidecar is read; the image itself is hashed.
     * @param error      Receives the reason on failure.
     * @return true when the module may be handed to the OS loader.
     */
    [[nodiscard]] bool ValidateModuleSidecar(const std::filesystem::path& modulePath, std::string& error);
} // namespace Spark::ModuleSidecar
