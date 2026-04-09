#include "AssetValidator.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>

namespace Spark
{

    void MaterialTextureValidator::ValidateAsset(const std::filesystem::path& path, ValidationReport& report)
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

        fs::path parentDir = path.parent_path();
        for (const auto& ext : {".png", ".jpg", ".dds", ".tga", ".bmp"})
        {
            fs::path texturePath = parentDir / (path.stem().string() + "_diffuse" + ext);
            if (fs::exists(texturePath, ec))
                return;
        }

        report.results.push_back({ValidationSeverity::Info, path.string(),
                                  "No matching diffuse texture found for material",
                                  "Ensure textures follow naming convention: <material>_diffuse.<ext>", 1003});
    }

    std::string_view MaterialTextureValidator::GetRuleName() const
    {
        return "MaterialTextureValidator";
    }

    void SceneReferenceValidator::ValidateAsset(const std::filesystem::path& path, ValidationReport& report)
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
        else if (fileSize > 100 * 1024 * 1024)
        {
            report.results.push_back({ValidationSeverity::Warning, path.string(),
                                      std::format("Scene file is very large ({:.1f} MB)", fileSize / (1024.0 * 1024.0)),
                                      "Consider splitting into streaming sub-scenes", 2003});
        }
    }

    std::string_view SceneReferenceValidator::GetRuleName() const
    {
        return "SceneReferenceValidator";
    }

    void ShaderCompilationValidator::ValidateAsset(const std::filesystem::path& path, ValidationReport& report)
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

    std::string_view ShaderCompilationValidator::GetRuleName() const
    {
        return "ShaderCompilationValidator";
    }

    void AssetMetadataValidator::ValidateAsset(const std::filesystem::path& path, ValidationReport& report)
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        if (!fs::exists(path, ec))
        {
            report.results.push_back({ValidationSeverity::Error, path.string(), "Asset file does not exist",
                                      "Remove stale reference", 4001});
            return;
        }

        std::string filename = path.filename().string();
        if (filename.size() > 200)
        {
            report.results.push_back({ValidationSeverity::Warning, path.string(),
                                      std::format("Filename is {} characters (max recommended: 200)", filename.size()),
                                      "Shorten the filename for cross-platform compatibility", 4002});
        }

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

        auto fileSize = fs::file_size(path, ec);
        if (!ec && fileSize == 0)
        {
            report.results.push_back({ValidationSeverity::Warning, path.string(), "File is empty (0 bytes)",
                                      "Populate or remove empty asset", 4004});
        }
    }

    std::string_view AssetMetadataValidator::GetRuleName() const
    {
        return "AssetMetadataValidator";
    }

    AssetValidator& AssetValidator::GetInstance()
    {
        static AssetValidator instance;
        return instance;
    }

    void AssetValidator::Initialize()
    {
        m_initialized = true;
        m_rules.clear();
        m_lastReport = {};

        m_rules.push_back(std::make_unique<MaterialTextureValidator>());
        m_rules.push_back(std::make_unique<SceneReferenceValidator>());
        m_rules.push_back(std::make_unique<ShaderCompilationValidator>());
        m_rules.push_back(std::make_unique<AssetMetadataValidator>());
    }

    void AssetValidator::Shutdown()
    {
        m_initialized = false;
        m_rules.clear();
        m_lastReport = {};
    }

    void AssetValidator::RegisterRule(std::unique_ptr<IAssetValidationRule> rule)
    {
        if (rule)
        {
            m_rules.push_back(std::move(rule));
        }
    }

    ValidationReport AssetValidator::ValidateAll()
    {
        return ValidateDirectory("Assets");
    }

    ValidationReport AssetValidator::ValidateFile(const std::filesystem::path& path)
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

    ValidationReport AssetValidator::ValidateDirectory(const std::filesystem::path& dir)
    {
        namespace fs = std::filesystem;
        auto startTime = std::chrono::steady_clock::now();

        ValidationReport report;
        report.timestamp = GetTimestamp();

        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
        {
            report.results.push_back({ValidationSeverity::Error, dir.string(),
                                      "Directory does not exist or is not a directory", "Verify the path and try again",
                                      9001});
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

    std::vector<std::string_view> AssetValidator::GetRegisteredRules() const
    {
        std::vector<std::string_view> names;
        names.reserve(m_rules.size());
        for (const auto& rule : m_rules)
        {
            names.push_back(rule->GetRuleName());
        }
        return names;
    }

    std::string AssetValidator::Console_GetStatus() const
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

    std::string AssetValidator::Console_GetLastReport() const
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

    std::string AssetValidator::GetTimestamp()
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

} // namespace Spark
