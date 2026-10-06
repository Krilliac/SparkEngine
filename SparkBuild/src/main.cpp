#include "Platform.h"
#include "SparkBuild.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef SPARK_BUILD_VERSION
#error "SPARK_BUILD_VERSION must be supplied by the build system"
#endif

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{
    struct PlanArguments
    {
        std::string enginePath;
        std::string buildPath;
        SparkBuild::Generator generator = SparkBuild::GetDefaultGenerator();
        SparkBuild::BuildType buildType = SparkBuild::BuildType::Release;
    };

    void PrintUsage()
    {
        std::cout << "SparkBuild - SparkEngine Build Tool v" SPARK_BUILD_VERSION "\n\n";
        std::cout << "Usage:\n";
        std::cout << "  sparkbuild              Run interactive TUI\n";
        std::cout << "  sparkbuild --help       Show this help\n";
        std::cout << "  sparkbuild --version    Show version\n";
        std::cout << "  sparkbuild --plan --engine-path <path> --build-path <path>\n";
        std::cout << "                       Print a validated, noninteractive build plan\n\n";
        std::cout << "Platform: " SPARK_PLATFORM_NAME "\n";
    }

    bool ConsumeValue(int& index, int argc, char* argv[], const char* option, std::string& value, std::string& error)
    {
        if (index + 1 >= argc)
        {
            error = std::string("Missing value for ") + option;
            return false;
        }
        value = argv[++index];
        if (value.empty())
        {
            error = std::string("Empty value for ") + option;
            return false;
        }
        return true;
    }

    bool ParseGenerator(const std::string& value, SparkBuild::Generator& generator)
    {
        if (value == "Ninja")
            generator = SparkBuild::Generator::Ninja;
        else if (value == "NinjaMultiConfig")
            generator = SparkBuild::Generator::NinjaMultiConfig;
        else if (value == "UnixMakefiles")
            generator = SparkBuild::Generator::UnixMakefiles;
#ifdef SPARK_PLATFORM_WINDOWS
        else if (value == "VS2022")
            generator = SparkBuild::Generator::VS2022;
        else if (value == "VS2026")
            generator = SparkBuild::Generator::VS2026;
#endif
        else
            return false;
        return true;
    }

    bool ParseBuildType(const std::string& value, SparkBuild::BuildType& buildType)
    {
        if (value == "Debug")
            buildType = SparkBuild::BuildType::Debug;
        else if (value == "Release")
            buildType = SparkBuild::BuildType::Release;
        else if (value == "RelWithDebInfo")
            buildType = SparkBuild::BuildType::RelWithDebInfo;
        else if (value == "MinSizeRel")
            buildType = SparkBuild::BuildType::MinSizeRel;
        else
            return false;
        return true;
    }

    int RunPlan(int argc, char* argv[])
    {
        PlanArguments args;
        std::string error;
        for (int index = 2; index < argc; ++index)
        {
            const std::string option = argv[index];
            std::string value;
            if (option == "--engine-path")
            {
                if (!ConsumeValue(index, argc, argv, "--engine-path", args.enginePath, error))
                    break;
            }
            else if (option == "--build-path")
            {
                if (!ConsumeValue(index, argc, argv, "--build-path", args.buildPath, error))
                    break;
            }
            else if (option == "--generator")
            {
                if (!ConsumeValue(index, argc, argv, "--generator", value, error))
                    break;
                if (!ParseGenerator(value, args.generator))
                    error = "Unsupported generator: " + value;
            }
            else if (option == "--build-type")
            {
                if (!ConsumeValue(index, argc, argv, "--build-type", value, error))
                    break;
                if (!ParseBuildType(value, args.buildType))
                    error = "Unsupported build type: " + value;
            }
            else
            {
                error = "Unknown --plan option: " + option;
            }
            if (!error.empty())
                break;
        }

        if (error.empty() && args.enginePath.empty())
            error = "--engine-path is required";
        if (error.empty() && args.buildPath.empty())
            error = "--build-path is required";

        if (!error.empty())
        {
            std::cerr << "SparkBuild plan error: " << error << "\n";
            return 2;
        }

        const std::filesystem::path enginePath = std::filesystem::path(args.enginePath);
        if (!std::filesystem::is_directory(enginePath) ||
            !std::filesystem::is_regular_file(enginePath / "CMakeLists.txt"))
        {
            std::cerr << "SparkBuild plan error: engine path must contain CMakeLists.txt: " << args.enginePath << "\n";
            return 2;
        }

        SparkBuild::ConfigManager config;
        config.config.enginePath = args.enginePath;
        config.config.buildPath = args.buildPath;
        config.config.generator = args.generator;
        config.config.buildType = args.buildType;

        try
        {
            std::cout << "SparkBuild plan v" SPARK_BUILD_VERSION "\n";
            std::cout << "EnginePath=" << args.enginePath << "\n";
            std::cout << "BuildPath=" << args.buildPath << "\n";
            std::cout << "Generator=" << SparkBuild::GeneratorToString(args.generator) << "\n";
            std::cout << "BuildType=" << SparkBuild::BuildTypeToString(args.buildType) << "\n";
            std::cout << "ConfigureCommand=" << config.BuildCMakeConfigureCommand() << "\n";
            std::cout << "BuildCommand=" << config.BuildCMakeBuildCommand() << "\n";
        }
        catch (const std::exception& exception)
        {
            std::cerr << "SparkBuild plan error: " << exception.what() << "\n";
            return 2;
        }
        return 0;
    }
} // namespace

int main(int argc, char* argv[])
{
#ifdef SPARK_PLATFORM_WINDOWS
    // Enable UTF-8 console output
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // Check for help/version flags
    if (argc > 1)
    {
        std::string arg = argv[1];
        if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return 0;
        }
        if (arg == "--version" || arg == "-v")
        {
            std::cout << "SparkBuild v" SPARK_BUILD_VERSION " (" SPARK_PLATFORM_NAME ")\n";
            return 0;
        }
        if (arg == "--plan")
            return RunPlan(argc, argv);
    }

    // Interactive TUI mode
    SparkBuild::SparkBuildApp app;
    return app.Run();
}
