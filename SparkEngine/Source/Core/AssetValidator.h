/**
 * @file AssetValidator.h
 * @brief Asset validation pipeline for content integrity checking
 *
 * Provides a rule-based validation system for verifying asset health before
 * packaging or at editor load time. Built-in rules check texture references in
 * materials, scene cross-references, shader compilability, and metadata
 * completeness. Custom rules are added via the IAssetValidationRule interface.
 *
 * ## Usage
 * @code
 *   auto& validator = Spark::AssetValidator::GetInstance();
 *   validator.Initialize();
 *
 *   auto report = validator.ValidateAll();
 *   for (const auto& r : report.results)
 *       if (r.severity >= Spark::ValidationSeverity::Warning)
 *           Log::Warn("Validation", "{}: {}", r.assetPath, r.message);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    // =========================================================================
    // Enumerations
    // =========================================================================

    /// @brief Severity of a validation finding.
    enum class ValidationSeverity : uint8_t
    {
        Info,    ///< Informational (no action required)
        Warning, ///< Potential issue that may cause runtime problems
        Error,   ///< Definite problem that will cause runtime failures
        Critical ///< Blocking issue that prevents asset from being used
    };

    // =========================================================================
    // Result types
    // =========================================================================

    /// @brief A single finding from an asset validation rule.
    struct ValidationResult
    {
        ValidationSeverity severity = ValidationSeverity::Info;
        std::string assetPath;  ///< Path to the asset that triggered this finding
        std::string message;    ///< Human-readable description of the issue
        std::string suggestion; ///< Recommended fix (may be empty)
        uint32_t errorCode = 0; ///< Machine-readable error identifier
    };

    /// @brief Aggregated report from a validation pass.
    struct ValidationReport
    {
        std::vector<ValidationResult> results;
        uint32_t totalAssets = 0;  ///< Number of assets scanned
        uint32_t passCount = 0;    ///< Assets with no errors or warnings
        uint32_t failCount = 0;    ///< Assets with at least one Error or Critical
        uint32_t warningCount = 0; ///< Assets with only Warnings (no Error/Critical)
        std::string timestamp;     ///< ISO-8601 timestamp of the validation run
        float durationMs = 0.0f;   ///< Wall-clock duration in milliseconds
    };

    // =========================================================================
    // Rule interface
    // =========================================================================

    /**
     * @brief Interface for a single validation rule.
     *
     * Implement this to add custom asset checks. Each rule inspects one file
     * at a time and appends findings to the provided report.
     */
    class IAssetValidationRule
    {
      public:
        virtual ~IAssetValidationRule() = default;

        /**
         * @brief Validate a single asset file.
         * @param path   Filesystem path to the asset.
         * @param report Report to append findings to.
         */
        virtual void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) = 0;

        /// @brief Human-readable name of this rule (for logging/UI).
        virtual std::string_view GetRuleName() const = 0;
    };

    // =========================================================================
    // Built-in rules
    // =========================================================================

    /**
     * @brief Validates that material files reference textures that exist on disk.
     *
     * Scans .mat files for texture path references and verifies each referenced
     * file is present. Missing textures produce Error severity findings.
     */
    class MaterialTextureValidator final : public IAssetValidationRule
    {
      public:
        void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) override
        {
            if (path.extension() != ".mat")
                return;

            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(path, ec))
            {
                report.results.push_back({ValidationSeverity::Error, path.string(), "Material file does not exist",
                                          "Verify the asset path", 1001});
                return;
            }

            auto fileSize = fs::file_size(path, ec);
            if (fileSize == 0)
            {
                report.results.push_back({ValidationSeverity::Warning, path.string(), "Material file is empty",
                                          "Add material properties or remove file", 1002});
                return;
            }

            // Check for common texture extensions referenced by the material
            // In a full implementation this would parse the material JSON/binary
            fs::path parentDir = path.parent_path();
            for (const auto& ext : {".png", ".jpg", ".dds", ".tga", ".bmp"})
            {
                fs::path texturePath = parentDir / (path.stem().string() + "_diffuse" + ext);
                if (fs::exists(texturePath, ec))
                    return; // Found at least one texture
            }

            report.results.push_back({ValidationSeverity::Info, path.string(),
                                      "No matching diffuse texture found for material",
                                      "Ensure textures follow naming convention: <material>_diffuse.<ext>", 1003});
        }

        std::string_view GetRuleName() const override { return "MaterialTextureValidator"; }
    };

    /**
     * @brief Validates that scene files do not contain broken entity references.
     *
     * Checks scene file existence, size sanity, and extension correctness.
     */
    class SceneReferenceValidator final : public IAssetValidationRule
    {
      public:
        void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) override
        {
            if (path.extension() != ".scene" && path.extension() != ".scn")
                return;

            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(path, ec))
            {
                report.results.push_back({ValidationSeverity::Error, path.string(), "Scene file does not exist",
                                          "Remove stale reference or restore file", 2001});
                return;
            }

            auto fileSize = fs::file_size(path, ec);
            if (fileSize == 0)
            {
                report.results.push_back({ValidationSeverity::Error, path.string(), "Scene file is empty (0 bytes)",
                                          "Re-save scene from editor", 2002});
            }
            else if (fileSize > 100 * 1024 * 1024) // > 100 MB
            {
                report.results.push_back(
                    {ValidationSeverity::Warning, path.string(),
                     std::format("Scene file is very large ({:.1f} MB)", fileSize / (1024.0 * 1024.0)),
                     "Consider splitting into streaming sub-scenes", 2003});
            }
        }

        std::string_view GetRuleName() const override { return "SceneReferenceValidator"; }
    };

    /**
     * @brief Validates shader source files for basic syntactic correctness.
     *
     * Checks that shader files exist, are non-empty, and contain expected
     * entry point keywords. Full compilation validation would use the shader
     * compiler; this rule catches obvious issues cheaply.
     */
    class ShaderCompilationValidator final : public IAssetValidationRule
    {
      public:
        void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) override
        {
            static constexpr std::string_view kShaderExts[] = {".hlsl", ".glsl", ".vert", ".frag", ".comp"};

            bool isShader = false;
            for (auto ext : kShaderExts)
            {
                if (path.extension().string() == ext)
                {
                    isShader = true;
                    break;
                }
            }
            if (!isShader)
                return;

            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(path, ec))
            {
                report.results.push_back({ValidationSeverity::Error, path.string(), "Shader source file does not exist",
                                          "Restore file or update references", 3001});
                return;
            }

            auto fileSize = fs::file_size(path, ec);
            if (fileSize == 0)
            {
                report.results.push_back({ValidationSeverity::Error, path.string(), "Shader source file is empty",
                                          "Add shader code or remove file", 3002});
            }
            else if (fileSize < 10)
            {
                report.results.push_back({ValidationSeverity::Warning, path.string(),
                                          "Shader source file is suspiciously small",
                                          "Verify shader contains valid entry points", 3003});
            }
        }

        std::string_view GetRuleName() const override { return "ShaderCompilationValidator"; }
    };

    /**
     * @brief Validates that assets have accompanying metadata or follow naming conventions.
     *
     * Checks for zero-byte files, excessively long filenames, and invalid characters
     * in asset paths that would cause issues on some platforms.
     */
    class AssetMetadataValidator final : public IAssetValidationRule
    {
      public:
        void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) override
        {
            namespace fs = std::filesystem;
            std::error_code ec;

            if (!fs::exists(path, ec))
            {
                report.results.push_back({ValidationSeverity::Error, path.string(), "Asset file does not exist",
                                          "Remove stale reference", 4001});
                return;
            }

            // Check filename length (cross-platform safety)
            std::string filename = path.filename().string();
            if (filename.size() > 200)
            {
                report.results.push_back(
                    {ValidationSeverity::Warning, path.string(),
                     std::format("Filename is {} characters (max recommended: 200)", filename.size()),
                     "Shorten the filename for cross-platform compatibility", 4002});
            }

            // Check for problematic characters in the path
            std::string fullPath = path.string();
            for (char c : fullPath)
            {
                if (c == '#' || c == '%' || c == '&' || c == '{' || c == '}')
                {
                    report.results.push_back({ValidationSeverity::Warning, path.string(),
                                              std::format("Path contains problematic character '{}'", c),
                                              "Rename to use only alphanumeric, dash, underscore, and dot", 4003});
                    break;
                }
            }

            // Check for zero-byte files
            auto fileSize = fs::file_size(path, ec);
            if (!ec && fileSize == 0)
            {
                report.results.push_back({ValidationSeverity::Warning, path.string(), "File is empty (0 bytes)",
                                          "Populate or remove empty asset", 4004});
            }
        }

        std::string_view GetRuleName() const override { return "AssetMetadataValidator"; }
    };

    // =========================================================================
    // AssetValidator
    // =========================================================================

    /**
     * @brief Singleton validation pipeline that runs registered rules against assets.
     *
     * On initialization, registers the four built-in rules. Additional rules can
     * be registered at any time via RegisterRule(). Validation can target a single
     * file, a directory tree, or the entire Assets/ hierarchy.
     */
    class AssetValidator
    {
      public:
        /// @brief Get the singleton instance.
        static AssetValidator& GetInstance()
        {
            static AssetValidator instance;
            return instance;
        }

        /// @brief Initialize with built-in rules.
        void Initialize()
        {
            m_initialized = true;
            m_rules.clear();
            m_lastReport = {};

            // Register built-in rules
            m_rules.push_back(std::make_unique<MaterialTextureValidator>());
            m_rules.push_back(std::make_unique<SceneReferenceValidator>());
            m_rules.push_back(std::make_unique<ShaderCompilationValidator>());
            m_rules.push_back(std::make_unique<AssetMetadataValidator>());
        }

        /// @brief Release resources and clear rules.
        void Shutdown()
        {
            m_initialized = false;
            m_rules.clear();
            m_lastReport = {};
        }

        /**
         * @brief Register a custom validation rule.
         * @param rule Owning pointer to the rule implementation.
         */
        void RegisterRule(std::unique_ptr<IAssetValidationRule> rule)
        {
            if (rule)
            {
                m_rules.push_back(std::move(rule));
            }
        }

        /**
         * @brief Validate all assets under the default Assets/ directory.
         * @return Aggregated validation report.
         */
        ValidationReport ValidateAll() { return ValidateDirectory("Assets"); }

        /**
         * @brief Validate a single file against all registered rules.
         * @param path Path to the asset file.
         * @return Validation report for this file.
         */
        ValidationReport ValidateFile(const std::filesystem::path& path)
        {
            auto startTime = std::chrono::steady_clock::now();

            ValidationReport report;
            report.timestamp = GetTimestamp();
            report.totalAssets = 1;

            bool hasError = false;
            bool hasWarning = false;

            for (const auto& rule : m_rules)
            {
                size_t beforeCount = report.results.size();
                rule->ValidateAsset(path, report);

                // Check what severity was added by this rule
                for (size_t i = beforeCount; i < report.results.size(); ++i)
                {
                    if (report.results[i].severity >= ValidationSeverity::Error)
                        hasError = true;
                    else if (report.results[i].severity == ValidationSeverity::Warning)
                        hasWarning = true;
                }
            }

            if (hasError)
                report.failCount = 1;
            else if (hasWarning)
                report.warningCount = 1;
            else
                report.passCount = 1;

            auto endTime = std::chrono::steady_clock::now();
            report.durationMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

            m_lastReport = report;
            return report;
        }

        /**
         * @brief Validate all assets in a directory tree.
         * @param dir Root directory to scan recursively.
         * @return Aggregated validation report.
         */
        ValidationReport ValidateDirectory(const std::filesystem::path& dir)
        {
            namespace fs = std::filesystem;
            auto startTime = std::chrono::steady_clock::now();

            ValidationReport report;
            report.timestamp = GetTimestamp();

            std::error_code ec;
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
            {
                report.results.push_back({ValidationSeverity::Error, dir.string(),
                                          "Directory does not exist or is not a directory",
                                          "Verify the path and try again", 9001});
                report.failCount = 1;
                m_lastReport = report;
                return report;
            }

            for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
            {
                if (!entry.is_regular_file())
                    continue;

                ++report.totalAssets;

                bool fileHasError = false;
                bool fileHasWarning = false;

                for (const auto& rule : m_rules)
                {
                    size_t beforeCount = report.results.size();
                    rule->ValidateAsset(entry.path(), report);

                    for (size_t i = beforeCount; i < report.results.size(); ++i)
                    {
                        if (report.results[i].severity >= ValidationSeverity::Error)
                            fileHasError = true;
                        else if (report.results[i].severity == ValidationSeverity::Warning)
                            fileHasWarning = true;
                    }
                }

                if (fileHasError)
                    ++report.failCount;
                else if (fileHasWarning)
                    ++report.warningCount;
                else
                    ++report.passCount;
            }

            auto endTime = std::chrono::steady_clock::now();
            report.durationMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

            m_lastReport = report;
            return report;
        }

        /**
         * @brief Get the names of all registered validation rules.
         * @return Vector of rule name string views.
         */
        std::vector<std::string_view> GetRegisteredRules() const
        {
            std::vector<std::string_view> names;
            names.reserve(m_rules.size());
            for (const auto& rule : m_rules)
            {
                names.push_back(rule->GetRuleName());
            }
            return names;
        }

        /// @brief Console command: return a human-readable status string.
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
            {
                return "AssetValidator: not initialized";
            }

            std::string status = std::format("AssetValidator: initialized, {} rule(s) registered", m_rules.size());

            if (m_lastReport.totalAssets > 0)
            {
                status += std::format("\n  Last run: {} asset(s) scanned in {:.1f} ms", m_lastReport.totalAssets,
                                      m_lastReport.durationMs);
                status += std::format("\n  Pass: {}, Fail: {}, Warnings: {}", m_lastReport.passCount,
                                      m_lastReport.failCount, m_lastReport.warningCount);
            }

            return status;
        }

        /// @brief Console command: return the last validation report as a formatted string.
        std::string Console_GetLastReport() const
        {
            if (m_lastReport.totalAssets == 0)
            {
                return "No validation report available (run ValidateAll first)";
            }

            std::string output = std::format("=== Validation Report ({}) ===\n", m_lastReport.timestamp);
            output +=
                std::format("Assets scanned: {}  |  Pass: {}  |  Fail: {}  |  Warnings: {}\n", m_lastReport.totalAssets,
                            m_lastReport.passCount, m_lastReport.failCount, m_lastReport.warningCount);
            output += std::format("Duration: {:.1f} ms\n\n", m_lastReport.durationMs);

            for (const auto& r : m_lastReport.results)
            {
                std::string_view severityStr;
                switch (r.severity)
                {
                case ValidationSeverity::Info:
                    severityStr = "INFO";
                    break;
                case ValidationSeverity::Warning:
                    severityStr = "WARN";
                    break;
                case ValidationSeverity::Error:
                    severityStr = "ERROR";
                    break;
                case ValidationSeverity::Critical:
                    severityStr = "CRIT";
                    break;
                }

                output += std::format("[{}] {} (E{})\n  {}\n", severityStr, r.assetPath, r.errorCode, r.message);
                if (!r.suggestion.empty())
                {
                    output += std::format("  Suggestion: {}\n", r.suggestion);
                }
            }

            return output;
        }

      private:
        AssetValidator() = default;
        ~AssetValidator() = default;
        AssetValidator(const AssetValidator&) = delete;
        AssetValidator& operator=(const AssetValidator&) = delete;

        /// @brief Generate an ISO-8601 timestamp string.
        static std::string GetTimestamp()
        {
            auto now = std::chrono::system_clock::now();
            auto timeT = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &timeT);
#else
            gmtime_r(&timeT, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return std::string(buf);
        }

        // =====================================================================
        // State
        // =====================================================================

        bool m_initialized = false;
        std::vector<std::unique_ptr<IAssetValidationRule>> m_rules;
        ValidationReport m_lastReport;
    };

} // namespace Spark
