#include "SparkAssetPipelineCore/AssetCooker.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    void PrintUsage()
    {
        std::cout << "Usage: SparkCooker --source <dir> --output <dir> [--manifest <file>] [--dry-run]\n";
    }
} // namespace

int main(int argc, char** argv)
{
    Spark::AssetPipeline::CookRequest request;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if ((argument == "--source" || argument == "--output" || argument == "--manifest") && index + 1 < argc)
        {
            const std::filesystem::path value = argv[++index];
            if (argument == "--source")
                request.sourceRoot = value;
            else if (argument == "--output")
                request.outputRoot = value;
            else
                request.manifestPath = value;
        }
        else if (argument == "--dry-run")
        {
            request.dryRun = true;
        }
        else if (argument == "--help" || argument == "-h")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            PrintUsage();
            return 2;
        }
    }
    if (request.sourceRoot.empty() || request.outputRoot.empty())
    {
        PrintUsage();
        return 2;
    }
    if (request.manifestPath.empty())
        request.manifestPath = request.outputRoot / "spark-cook-manifest.json";
    request.onProgress = [](const Spark::AssetPipeline::CookRecord& record, std::size_t current, std::size_t total)
    {
        std::cout << '[' << current << '/' << total << "] " << (record.updated ? "cooked " : "unchanged ")
                  << record.path << '\n'
                  << std::flush;
    };

    const auto result = Spark::AssetPipeline::CookAssets(request);
    if (!result.Succeeded())
    {
        std::cerr << "SparkCooker: " << result.error << '\n';
        return 1;
    }
    std::cout << "SparkCooker: " << result.records.size() << " asset(s), " << result.updatedCount << " updated, "
              << result.unchangedCount << " unchanged\nmanifest-sha256 " << result.manifestSha256 << '\n';
    return 0;
}
