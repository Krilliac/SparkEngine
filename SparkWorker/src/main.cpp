#include "SparkAssetPipelineCore/AssetCooker.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    std::filesystem::path source;
    std::filesystem::path output;
    std::string expected;
    bool dryRun = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if ((argument == "--source" || argument == "--output" || argument == "--sha256") && index + 1 < argc)
        {
            const std::string value = argv[++index];
            if (argument == "--source")
                source = value;
            else if (argument == "--output")
                output = value;
            else
                expected = value;
        }
        else if (argument == "--dry-run")
            dryRun = true;
        else
        {
            std::cerr << "Usage: SparkWorker --source <file> --output <file> --sha256 <digest> [--dry-run]\n";
            return argument == "--help" || argument == "-h" ? 0 : 2;
        }
    }
    if (source.empty() || output.empty() || expected.size() != 64)
    {
        std::cerr << "SparkWorker requires source, output, and a 64-character SHA-256 digest.\n";
        return 2;
    }
    std::string actual;
    std::string error;
    if (!Spark::AssetPipeline::ComputeFileSha256(source, actual, error) || actual != expected)
    {
        std::cerr << (error.empty() ? "source digest does not match the requested job" : error) << '\n';
        return 1;
    }
    bool updated = false;
    if (!Spark::AssetPipeline::CookFile(source, output, expected, dryRun, updated, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << (updated ? "updated" : "unchanged") << " " << output.generic_string() << '\n';
    return 0;
}
