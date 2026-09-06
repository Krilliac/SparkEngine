/**
 * @file VirtualFileSystem.h
 * @brief Mount-priority virtual filesystem for layered content loading
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Provides a virtual filesystem that layers engine, game, DLC, and mod content
 * through prioritized mount points. Higher priority mounts override lower ones,
 * allowing mods to transparently replace engine/game assets without modifying
 * the originals.
 *
 * ## Mount priorities
 * ```
 * ENGINE (0)  <  GAME (100)  <  DLC (200)  <  MOD (300)
 * ```
 *
 * ## Usage
 * @code
 *   auto& vfs = VirtualFileSystem::GetInstance();
 *   vfs.Initialize();
 *   vfs.Mount("engine", std::make_unique<LocalFileProvider>("Data/Engine"), ENGINE_PRIORITY);
 *   vfs.Mount("game",   std::make_unique<LocalFileProvider>("Data/Game"),   GAME_PRIORITY);
 *
 *   auto data = vfs.ReadFile("textures/player.png");
 *   // Returns the game version if it exists, otherwise the engine version.
 * @endcode
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    // =========================================================================
    // Virtual path policy
    // =========================================================================

    /**
     * @brief Whether a mount-relative virtual path may be resolved at all.
     *
     * A substring test for ".." is both too weak and too strong on Windows. It
     * rejects legitimate names ("weapon..old.mesh") while missing:
     *  - drive-relative paths ("C:evil"),
     *  - NTFS alternate data streams ("assets/logo.png:secret"),
     *  - reserved device names ("COM1", "NUL"), which Win32 resolves as devices
     *    no matter what directory prefix they carry, so opening one can block the
     *    loading thread on a serial port.
     *
     * This predicate decides containment on the NORMALIZED path, so "a/../../etc"
     * is rejected for the '..' it actually resolves to rather than for the two
     * characters it contains. It is the policy gate for every untrusted path the
     * engine consumes — VFS mounts and .sparkscene manifests alike.
     *
     * @param virtualPath Mount-relative path from untrusted content.
     * @return true when the path is relative, contains no escaping component, no
     *         ':' and no reserved device name.
     */
    [[nodiscard]] bool IsVirtualPathSafe(const std::string& virtualPath);

    // =========================================================================
    // Priority constants
    // =========================================================================

    constexpr int32_t ENGINE_PRIORITY = 0;
    constexpr int32_t GAME_PRIORITY = 100;
    constexpr int32_t DLC_PRIORITY = 200;
    constexpr int32_t MOD_PRIORITY = 300;

    // =========================================================================
    // IResourceProvider
    // =========================================================================

    /**
     * @brief Interface for resource providers (filesystem, archive, network).
     *
     * Implementations provide read-only access to files under a conceptual
     * root. The virtual filesystem queries providers in priority order.
     */
    class IResourceProvider
    {
      public:
        virtual ~IResourceProvider() = default;

        /// @brief Check whether a file exists at the given virtual path.
        virtual bool Exists(const std::string& virtualPath) const = 0;

        /// @brief Read the entire file as a binary blob.
        virtual std::vector<uint8_t> ReadFile(const std::string& virtualPath) const = 0;

        /// @brief Read the entire file as a UTF-8 string.
        virtual std::string ReadTextFile(const std::string& virtualPath) const = 0;

        /// @brief List files in a directory, optionally filtered by extension (e.g. ".png").
        virtual std::vector<std::string> ListFiles(const std::string& directory,
                                                   const std::string& extension = "") const = 0;

        /// @brief Human-readable name for this provider.
        virtual std::string GetProviderName() const = 0;
    };

    // =========================================================================
    // LocalFileProvider
    // =========================================================================

    /**
     * @brief Filesystem-backed resource provider.
     *
     * Maps virtual paths to real filesystem paths under a root directory.
     */
    class LocalFileProvider : public IResourceProvider
    {
      public:
        /// @brief Construct a provider rooted at the given directory.
        explicit LocalFileProvider(const std::string& rootPath);

        bool Exists(const std::string& virtualPath) const override;
        std::vector<uint8_t> ReadFile(const std::string& virtualPath) const override;
        std::string ReadTextFile(const std::string& virtualPath) const override;
        std::vector<std::string> ListFiles(const std::string& directory,
                                           const std::string& extension = "") const override;
        std::string GetProviderName() const override;

      private:
        /// @brief Resolve a virtual path to the real filesystem path, or {} if it escapes the root.
        std::string ResolvePath(const std::string& virtualPath) const;

        std::string m_rootPath;
        std::filesystem::path m_root;          ///< Normalized root, no trailing separator.
        std::filesystem::path m_canonicalRoot; ///< Root with symlinks resolved, for the containment check.
    };

    // =========================================================================
    // MountPoint
    // =========================================================================

    /**
     * @brief A named, prioritized resource provider mount.
     */
    struct MountPoint
    {
        std::string name;
        std::unique_ptr<IResourceProvider> provider;
        int32_t priority = 0;
    };

    // =========================================================================
    // VirtualFileSystem
    // =========================================================================

    /**
     * @brief Singleton mount-priority virtual filesystem.
     *
     * Mounts are queried in descending priority order so that higher-priority
     * providers (mods, DLC) transparently override lower-priority ones (engine).
     */
    class VirtualFileSystem
    {
      public:
        /// @brief Get the singleton instance.
        static VirtualFileSystem& GetInstance();

        VirtualFileSystem(const VirtualFileSystem&) = delete;
        VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

        /// @brief Initialize the virtual filesystem.
        bool Initialize();

        /// @brief Shut down and release all mounts.
        void Shutdown();

        /// @brief Add a named mount point with the given priority.
        void Mount(const std::string& name, std::unique_ptr<IResourceProvider> provider, int32_t priority);

        /// @brief Remove a mount point by name.
        void Unmount(const std::string& name);

        /// @brief Check whether any mount contains the given file.
        bool Exists(const std::string& virtualPath) const;

        /// @brief Read a file as binary from the highest-priority mount that contains it.
        std::vector<uint8_t> ReadFile(const std::string& virtualPath) const;

        /// @brief Read a file as text from the highest-priority mount that contains it.
        std::string ReadTextFile(const std::string& virtualPath) const;

        /// @brief List files across all mounts, merging results (no duplicates).
        std::vector<std::string> ListFiles(const std::string& directory, const std::string& extension = "") const;

        /// @brief Return the provider name that owns a file (highest-priority match).
        std::string ResolveProvider(const std::string& virtualPath) const;

        /// @brief Number of active mount points.
        uint32_t GetMountCount() const;

        /// @brief Console status string listing all mounts and priorities.
        std::string Console_GetStatus() const;

      private:
        VirtualFileSystem() = default;
        ~VirtualFileSystem() = default;

        /// @brief Get a snapshot of mounts sorted by descending priority.
        std::vector<const MountPoint*> GetSortedMounts() const;

        mutable std::mutex m_mutex;
        std::vector<MountPoint> m_mounts;
        bool m_initialized = false;
    };

} // namespace Spark
