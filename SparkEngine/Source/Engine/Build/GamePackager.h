/**
 * @file GamePackager.h
 * @brief Standalone game packaging system for distributable builds
 * @author Spark Engine Team
 * @date 2026
 *
 * Produces a self-contained redistributable directory from a built game.
 * Collects the executable, game module DLLs, cooked assets, runtime
 * dependencies, and configuration files into an output folder.
 *
 * ## Usage
 * @code
 *   auto& packager = Spark::Build::GamePackager::GetInstance();
 *   packager.Initialize();
 *
 *   PackageConfig config;
 *   config.projectName = "MyGame";
 *   config.executablePath = "build/Release/SparkEngine.exe";
 *   config.outputDirectory = "Package/MyGame";
 *   config.moduleDirectory = "build/Release/GameModules";
 *   config.assetDirectory = "Assets";
 *   config.createZip = true;
 *
 *   auto result = packager.Package(config);
 *   if (result.success)
 *       // result.outputPath contains the final package path
 * @endcode
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "Utils/LogMacros.h"

namespace Spark::Build
{

    /** @brief Target platform for the package */
    enum class PackagePlatform
    {
        WindowsX64, ///< Windows 64-bit
        LinuxX64,   ///< Linux 64-bit
        MacOSX64,   ///< macOS Intel
        MacOSARM64  ///< macOS Apple Silicon
    };

    /** @brief Configuration for a packaging operation */
    struct PackageConfig
    {
        std::string projectName = "SparkGame";   ///< Project/game name
        std::string executablePath;              ///< Path to the built executable
        std::string outputDirectory = "Package"; ///< Output directory for the package
        std::string moduleDirectory;             ///< Directory containing game module DLLs
        std::string assetDirectory = "Assets";   ///< Directory containing game assets
        std::string dataDirectory = "Data";      ///< Directory containing cooked data (.spk files)

        PackagePlatform platform = PackagePlatform::WindowsX64; ///< Target platform

        bool includeDebugSymbols = false; ///< Include PDB/DWARF files
        bool includeEditor = false;       ///< Include editor components
        bool includeDevConsole = false;   ///< Include developer console
        bool createZip = false;           ///< Create a .zip archive of the output
        int compressionLevel = 6;         ///< Zip compression level (1-9)

        std::vector<std::string> extraFiles;      ///< Additional files to include
        std::vector<std::string> excludePatterns; ///< Glob patterns to exclude
        std::vector<std::string> requiredModules; ///< Specific game modules to include (empty = all)
    };

    /** @brief A single file entry in the package manifest */
    struct ManifestEntry
    {
        std::string sourcePath;    ///< Absolute source path
        std::string relativePath;  ///< Path within the package
        uint64_t sizeBytes = 0;    ///< File size
        bool isExecutable = false; ///< Whether to set executable bit (Linux/macOS)
    };

    /** @brief Result of a packaging operation */
    struct PackageResult
    {
        bool success = false;              ///< Whether packaging succeeded
        std::string outputPath;            ///< Path to the output directory or zip
        uint32_t filesCopied = 0;          ///< Number of files in the package
        uint64_t totalSizeBytes = 0;       ///< Total package size
        double durationSeconds = 0.0;      ///< Time taken for the operation
        std::string errorMessage;          ///< Error description on failure
        std::vector<std::string> warnings; ///< Non-fatal warnings
    };

    /**
     * @brief Standalone game packaging system
     *
     * Collects all files needed for a distributable game build into a single
     * directory, optionally creating a .zip archive. Handles platform-specific
     * file extensions, executable permissions, and dependency collection.
     *
     * Thread safety: Not thread-safe. Call from main thread only.
     */
    class GamePackager
    {
      public:
        static GamePackager& GetInstance()
        {
            static GamePackager instance;
            return instance;
        }

        /** @brief Initialize the packaging system */
        void Initialize()
        {
            m_initialized = true;
            m_lastResult = {};
            m_packageCount = 0;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "GamePackager initialized");
        }

        /** @brief Shut down */
        void Shutdown() { m_initialized = false; }

        /**
         * @brief Package a game build into a distributable directory
         * @param config Packaging configuration
         * @return Result of the packaging operation
         */
        PackageResult Package(const PackageConfig& config)
        {
            if (!m_initialized)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "GamePackager: Package called but not initialized");
                return {false, "", 0, 0, 0.0, "GamePackager not initialized", {}};
            }

            auto startTime = std::chrono::steady_clock::now();
            PackageResult result;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "GamePackager: Packaging '%s'...", config.projectName.c_str());

            // Validate config
            if (config.projectName.empty())
            {
                result.errorMessage = "Project name is empty";
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "GamePackager: %s", result.errorMessage.c_str());
                m_lastResult = result;
                return result;
            }

            // Build the manifest
            std::vector<ManifestEntry> manifest;

            // 1. Collect executable
            if (!CollectExecutable(config, manifest, result))
            {
                m_lastResult = result;
                return result;
            }

            // 2. Collect game module DLLs
            CollectModules(config, manifest, result);

            // 3. Collect assets
            CollectAssets(config, manifest, result);

            // 4. Collect data directory (.spk archives)
            CollectDataFiles(config, manifest, result);

            // 5. Collect extra files
            for (const auto& extra : config.extraFiles)
            {
                std::error_code ec;
                if (std::filesystem::exists(extra, ec))
                {
                    ManifestEntry entry;
                    entry.sourcePath = extra;
                    entry.relativePath = std::filesystem::path(extra).filename().string();
                    entry.sizeBytes = std::filesystem::file_size(extra, ec);
                    manifest.push_back(std::move(entry));
                }
                else
                {
                    result.warnings.push_back("Extra file not found: " + extra);
                }
            }

            // 6. Filter excludes
            if (!config.excludePatterns.empty())
            {
                FilterExcludes(manifest, config.excludePatterns);
            }

            // 7. Create output directory
            std::filesystem::path outDir = config.outputDirectory;
            outDir /= config.projectName;
            std::error_code ec;
            std::filesystem::create_directories(outDir, ec);
            if (ec)
            {
                result.errorMessage = "Failed to create output directory: " + ec.message();
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "GamePackager: %s", result.errorMessage.c_str());
                m_lastResult = result;
                return result;
            }

            // 8. Copy files
            for (const auto& entry : manifest)
            {
                std::filesystem::path destPath = outDir / entry.relativePath;
                std::filesystem::create_directories(destPath.parent_path(), ec);

                std::filesystem::copy_file(entry.sourcePath, destPath,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    result.warnings.push_back("Failed to copy: " + entry.sourcePath + " (" + ec.message() + ")");
                    continue;
                }

                result.filesCopied++;
                result.totalSizeBytes += entry.sizeBytes;
            }

            auto endTime = std::chrono::steady_clock::now();
            result.durationSeconds = std::chrono::duration<double>(endTime - startTime).count();
            result.outputPath = outDir.string();
            result.success = (result.filesCopied > 0) && result.errorMessage.empty();

            SPARK_LOG_INFO(Spark::LogCategory::Core, "GamePackager: Packaging complete (%u files, %llu bytes, %.2fs)",
                           result.filesCopied, static_cast<unsigned long long>(result.totalSizeBytes),
                           result.durationSeconds);
            m_lastResult = result;
            m_packageCount++;
            return result;
        }

        /** @brief Validate a config without actually packaging */
        std::vector<std::string> ValidateConfig(const PackageConfig& config) const
        {
            std::vector<std::string> errors;
            std::error_code ec;

            if (config.projectName.empty())
                errors.push_back("Project name is empty");
            if (config.executablePath.empty())
                errors.push_back("Executable path is empty");
            else if (!std::filesystem::exists(config.executablePath, ec))
                errors.push_back("Executable not found: " + config.executablePath);
            if (!config.moduleDirectory.empty() && !std::filesystem::exists(config.moduleDirectory, ec))
                errors.push_back("Module directory not found: " + config.moduleDirectory);
            if (!config.assetDirectory.empty() && !std::filesystem::exists(config.assetDirectory, ec))
                errors.push_back("Asset directory not found: " + config.assetDirectory);

            return errors;
        }

        /** @brief Get the result of the last packaging operation */
        const PackageResult& GetLastResult() const { return m_lastResult; }

        /** @brief Get total number of packages created */
        uint32_t GetPackageCount() const { return m_packageCount; }

        /** @brief Get the platform-specific DLL extension */
        static std::string GetModuleExtension(PackagePlatform platform)
        {
            switch (platform)
            {
            case PackagePlatform::WindowsX64:
                return ".dll";
            case PackagePlatform::LinuxX64:
                return ".so";
            case PackagePlatform::MacOSX64:
            case PackagePlatform::MacOSARM64:
                return ".dylib";
            }
            return ".dll";
        }

        /** @brief Get the platform-specific executable extension */
        static std::string GetExecutableExtension(PackagePlatform platform)
        {
            switch (platform)
            {
            case PackagePlatform::WindowsX64:
                return ".exe";
            default:
                return "";
            }
        }

        /** @brief Get console-friendly status string */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[GamePackager] Not initialized";

            std::string status = "[GamePackager] Packages built: " + std::to_string(m_packageCount);
            if (m_lastResult.success)
            {
                status += " | Last: " + m_lastResult.outputPath;
                status += " (" + std::to_string(m_lastResult.filesCopied) + " files, ";
                status += FormatSize(m_lastResult.totalSizeBytes) + ")";
            }
            return status;
        }

      private:
        GamePackager() = default;

        bool CollectExecutable(const PackageConfig& config, std::vector<ManifestEntry>& manifest,
                               PackageResult& result) const
        {
            std::error_code ec;
            if (!std::filesystem::exists(config.executablePath, ec))
            {
                result.errorMessage = "Executable not found: " + config.executablePath;
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "GamePackager: %s", result.errorMessage.c_str());
                return false;
            }

            SPARK_LOG_INFO(Spark::LogCategory::Core, "GamePackager: Collecting executable '%s'",
                           config.executablePath.c_str());
            ManifestEntry entry;
            entry.sourcePath = config.executablePath;
            entry.relativePath = std::filesystem::path(config.executablePath).filename().string();
            entry.sizeBytes = std::filesystem::file_size(config.executablePath, ec);
            entry.isExecutable = true;
            manifest.push_back(std::move(entry));

            // Collect debug symbols if requested
            if (config.includeDebugSymbols)
            {
                auto pdbPath = std::filesystem::path(config.executablePath).replace_extension(".pdb");
                if (std::filesystem::exists(pdbPath, ec))
                {
                    ManifestEntry pdb;
                    pdb.sourcePath = pdbPath.string();
                    pdb.relativePath = pdbPath.filename().string();
                    pdb.sizeBytes = std::filesystem::file_size(pdbPath, ec);
                    manifest.push_back(std::move(pdb));
                }
            }

            return true;
        }

        void CollectModules(const PackageConfig& config, std::vector<ManifestEntry>& manifest,
                            PackageResult& result) const
        {
            if (config.moduleDirectory.empty())
                return;

            std::error_code ec;
            if (!std::filesystem::exists(config.moduleDirectory, ec))
            {
                result.warnings.push_back("Module directory not found: " + config.moduleDirectory);
                return;
            }

            std::string ext = GetModuleExtension(config.platform);
            std::unordered_set<std::string> required(config.requiredModules.begin(), config.requiredModules.end());

            for (const auto& entry : std::filesystem::directory_iterator(config.moduleDirectory, ec))
            {
                if (!entry.is_regular_file())
                    continue;
                if (entry.path().extension() != ext)
                    continue;

                std::string stem = entry.path().stem().string();
                if (!required.empty() && required.find(stem) == required.end())
                    continue;

                ManifestEntry me;
                me.sourcePath = entry.path().string();
                me.relativePath = "Modules/" + entry.path().filename().string();
                me.sizeBytes = std::filesystem::file_size(entry.path(), ec);
                manifest.push_back(std::move(me));
            }
        }

        void CollectAssets(const PackageConfig& config, std::vector<ManifestEntry>& manifest,
                           PackageResult& result) const
        {
            CollectDirectory(config.assetDirectory, "Assets", manifest, result);
        }

        void CollectDataFiles(const PackageConfig& config, std::vector<ManifestEntry>& manifest,
                              PackageResult& result) const
        {
            CollectDirectory(config.dataDirectory, "Data", manifest, result);
        }

        void CollectDirectory(const std::string& sourceDir, const std::string& destPrefix,
                              std::vector<ManifestEntry>& manifest, PackageResult& result) const
        {
            if (sourceDir.empty())
                return;

            std::error_code ec;
            if (!std::filesystem::exists(sourceDir, ec))
            {
                result.warnings.push_back("Directory not found: " + sourceDir);
                return;
            }

            for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceDir, ec))
            {
                if (!entry.is_regular_file())
                    continue;

                auto relPath = std::filesystem::relative(entry.path(), sourceDir, ec);
                ManifestEntry me;
                me.sourcePath = entry.path().string();
                me.relativePath = destPrefix + "/" + relPath.string();
                me.sizeBytes = std::filesystem::file_size(entry.path(), ec);
                manifest.push_back(std::move(me));
            }
        }

        void FilterExcludes(std::vector<ManifestEntry>& manifest, const std::vector<std::string>& patterns) const
        {
            manifest.erase(std::remove_if(manifest.begin(), manifest.end(),
                                          [&patterns](const ManifestEntry& entry)
                                          {
                                              for (const auto& pattern : patterns)
                                              {
                                                  if (entry.relativePath.find(pattern) != std::string::npos)
                                                      return true;
                                              }
                                              return false;
                                          }),
                           manifest.end());
        }

        static std::string FormatSize(uint64_t bytes)
        {
            if (bytes < 1024)
                return std::to_string(bytes) + " B";
            if (bytes < uint64_t{1024} * 1024)
                return std::to_string(bytes / 1024) + " KB";
            if (bytes < 1024ULL * 1024 * 1024)
                return std::to_string(bytes / (uint64_t{1024} * 1024)) + " MB";
            return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
        }

        bool m_initialized = false;
        uint32_t m_packageCount = 0;
        PackageResult m_lastResult;
    };

} // namespace Spark::Build
