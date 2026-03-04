/**
 * @file main.cpp
 * @brief SparkShaderCompiler - Standalone offline shader compilation tool
 * @author Spark Engine Team
 * @date 2025
 *
 * Compiles HLSL/GLSL shaders for all supported backends using the
 * RHI cross-compilation pipeline (RHIFactory::CompileShader).
 *
 * Usage:
 *   SparkShaderCompiler <input> [options]
 *
 * Options:
 *   -o <path>        Output file path
 *   -stage <stage>   Shader stage: vertex, pixel, geometry, hull, domain, compute
 *   -backend <api>   Target backend: d3d11, vulkan, opengl, auto (default: auto)
 *   -entry <name>    Entry point function name (default: main)
 *   -D<DEFINE>       Add preprocessor define
 *   -I<path>         Add include search path
 *   -O               Enable optimization (default)
 *   -Od              Disable optimization
 *   -Zi              Enable debug info
 *   -validate        Validate only, don't write output
 *   -reflect         Print shader reflection data
 *   -batch <dir>     Compile all shaders in directory
 *   -v               Verbose output
 *   -h, --help       Show help
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

// Include the RHI shader compilation API
#include "../../Spark Engine/Source/Graphics/RHI/RHIFactory.h"
#include "../../Spark Engine/Source/Graphics/RHI/RHITypes.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

struct CompilerConfig
{
    std::string inputFile;
    std::string outputFile;
    std::string entryPoint = "main";
    Spark::RHI::RHIShaderStage stage = Spark::RHI::RHIShaderStage::Vertex;
    Spark::RHI::GraphicsBackend targetBackend = Spark::RHI::GraphicsBackend::Auto;
    Spark::RHI::ShaderLanguage sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    std::vector<std::string> defines;
    std::vector<std::string> includePaths;
    bool optimization = true;
    bool debugInfo = false;
    bool validateOnly = false;
    bool reflect = false;
    bool verbose = false;
    std::string batchDir;
};

// ============================================================================
// HELPERS
// ============================================================================

static void PrintUsage(const char* programName)
{
    std::cout << "SparkShaderCompiler - Spark Engine Offline Shader Compiler\n"
              << "\n"
              << "Usage: " << programName << " <input> [options]\n"
              << "\n"
              << "Options:\n"
              << "  -o <path>        Output file path\n"
              << "  -stage <stage>   Shader stage: vertex, pixel, geometry, hull, domain, compute\n"
              << "  -backend <api>   Target backend: d3d11, vulkan, opengl, auto (default: auto)\n"
              << "  -entry <name>    Entry point (default: main)\n"
              << "  -D<DEFINE>       Add preprocessor define\n"
              << "  -I<path>         Add include search path\n"
              << "  -O               Enable optimization (default)\n"
              << "  -Od              Disable optimization\n"
              << "  -Zi              Enable debug info\n"
              << "  -validate        Validate only, don't write output\n"
              << "  -reflect         Print shader reflection data\n"
              << "  -batch <dir>     Compile all shaders in directory\n"
              << "  -v               Verbose output\n"
              << "  -h, --help       Show this help\n"
              << "\n"
              << "Examples:\n"
              << "  " << programName << " BasicVS.hlsl -stage vertex -backend d3d11 -o BasicVS.cso\n"
              << "  " << programName << " PBR.hlsl -stage pixel -backend vulkan -o PBR.spv\n"
              << "  " << programName << " Water.glsl -stage vertex -backend opengl -validate\n"
              << "  " << programName << " -batch Shaders/HLSL -backend vulkan\n";
}

static Spark::RHI::RHIShaderStage ParseStage(const std::string& str)
{
    if (str == "vertex"   || str == "vert" || str == "vs") return Spark::RHI::RHIShaderStage::Vertex;
    if (str == "pixel"    || str == "frag" || str == "ps") return Spark::RHI::RHIShaderStage::Pixel;
    if (str == "geometry" || str == "geom" || str == "gs") return Spark::RHI::RHIShaderStage::Geometry;
    if (str == "hull"     || str == "hs")                  return Spark::RHI::RHIShaderStage::Hull;
    if (str == "domain"   || str == "ds")                  return Spark::RHI::RHIShaderStage::Domain;
    if (str == "compute"  || str == "cs")                  return Spark::RHI::RHIShaderStage::Compute;
    std::cerr << "Warning: Unknown shader stage '" << str << "', defaulting to vertex\n";
    return Spark::RHI::RHIShaderStage::Vertex;
}

static Spark::RHI::GraphicsBackend ParseBackend(const std::string& str)
{
    if (str == "d3d11"  || str == "dx11")   return Spark::RHI::GraphicsBackend::D3D11;
    if (str == "d3d12"  || str == "dx12")   return Spark::RHI::GraphicsBackend::D3D12;
    if (str == "vulkan" || str == "vk")     return Spark::RHI::GraphicsBackend::Vulkan;
    if (str == "opengl" || str == "gl")     return Spark::RHI::GraphicsBackend::OpenGL;
    if (str == "auto")                      return Spark::RHI::GraphicsBackend::Auto;
    std::cerr << "Warning: Unknown backend '" << str << "', defaulting to auto\n";
    return Spark::RHI::GraphicsBackend::Auto;
}

static const char* StageToString(Spark::RHI::RHIShaderStage stage)
{
    switch (stage) {
        case Spark::RHI::RHIShaderStage::Vertex:   return "Vertex";
        case Spark::RHI::RHIShaderStage::Pixel:    return "Pixel";
        case Spark::RHI::RHIShaderStage::Geometry:  return "Geometry";
        case Spark::RHI::RHIShaderStage::Hull:     return "Hull";
        case Spark::RHI::RHIShaderStage::Domain:   return "Domain";
        case Spark::RHI::RHIShaderStage::Compute:  return "Compute";
        default:                                   return "Unknown";
    }
}

static std::string InferOutputPath(const std::string& inputFile,
                                   Spark::RHI::GraphicsBackend backend)
{
    std::string base = inputFile;
    size_t dotPos = base.find_last_of('.');
    if (dotPos != std::string::npos)
        base = base.substr(0, dotPos);

    const char* ext = Spark::RHI::GetShaderExtension(backend);
    if (backend == Spark::RHI::GraphicsBackend::Vulkan)
        return base + ".spv";
    if (backend == Spark::RHI::GraphicsBackend::D3D11 ||
        backend == Spark::RHI::GraphicsBackend::D3D12)
        return base + ".cso";
    return base + ext;
}

static Spark::RHI::RHIShaderStage InferStageFromFilename(const std::string& filename)
{
    // Common naming conventions
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("vs") != std::string::npos || lower.find("vert") != std::string::npos)
        return Spark::RHI::RHIShaderStage::Vertex;
    if (lower.find("ps") != std::string::npos || lower.find("frag") != std::string::npos ||
        lower.find("pixel") != std::string::npos)
        return Spark::RHI::RHIShaderStage::Pixel;
    if (lower.find("gs") != std::string::npos || lower.find("geom") != std::string::npos)
        return Spark::RHI::RHIShaderStage::Geometry;
    if (lower.find("hs") != std::string::npos || lower.find("hull") != std::string::npos)
        return Spark::RHI::RHIShaderStage::Hull;
    if (lower.find("ds") != std::string::npos || lower.find("domain") != std::string::npos)
        return Spark::RHI::RHIShaderStage::Domain;
    if (lower.find("cs") != std::string::npos || lower.find("compute") != std::string::npos)
        return Spark::RHI::RHIShaderStage::Compute;

    // Default
    return Spark::RHI::RHIShaderStage::Vertex;
}

// ============================================================================
// COMPILATION
// ============================================================================

static int CompileSingleShader(const CompilerConfig& config)
{
    if (config.verbose) {
        std::cout << "Compiling: " << config.inputFile << "\n"
                  << "  Stage:   " << StageToString(config.stage) << "\n"
                  << "  Backend: " << Spark::RHI::GetBackendName(config.targetBackend) << "\n"
                  << "  Entry:   " << config.entryPoint << "\n"
                  << "  Opt:     " << (config.optimization ? "on" : "off") << "\n"
                  << "  Debug:   " << (config.debugInfo ? "on" : "off") << "\n";
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    // Build compile options
    Spark::RHI::ShaderCompileOptions options;
    options.stage = config.stage;
    options.sourceFile = config.inputFile;
    options.entryPoint = config.entryPoint;
    options.targetBackend = config.targetBackend;
    options.sourceLanguage = config.sourceLanguage;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.optimizationEnabled = config.optimization;
    options.debugInfoEnabled = config.debugInfo;
    options.defines = config.defines;
    options.includePaths = config.includePaths;
    options.generateReflection = config.reflect;

    // Compile through RHI pipeline
    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    auto endTime = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    if (!result.success) {
        std::cerr << "ERROR: Compilation failed for " << config.inputFile << "\n";
        if (!result.errorMessage.empty())
            std::cerr << "  " << result.errorMessage << "\n";
        return 1;
    }

    if (!result.warningMessage.empty()) {
        std::cerr << "WARNING: " << result.warningMessage << "\n";
    }

    if (config.verbose) {
        std::cout << "  Compiled in " << elapsed << " ms"
                  << " (" << result.bytecode.size() << " bytes)\n";
    }

    // Reflection
    if (config.reflect && !result.bytecode.empty()) {
        auto reflection = Spark::RHI::ReflectSPIRV(result.bytecode);
        std::cout << "\nShader Reflection:\n";
        std::cout << "  Inputs: " << reflection.inputs.size() << "\n";
        for (const auto& input : reflection.inputs) {
            std::cout << "    location=" << input.location
                      << " " << input.semanticName << "\n";
        }
        std::cout << "  Resources: " << reflection.resources.size() << "\n";
        for (const auto& res : reflection.resources) {
            std::cout << "    set=" << res.set << " binding=" << res.binding
                      << " " << res.name << " (size=" << res.size << ")\n";
        }
    }

    // Write output
    if (!config.validateOnly) {
        std::string outputPath = config.outputFile;
        if (outputPath.empty())
            outputPath = InferOutputPath(config.inputFile, config.targetBackend);

        if (!Spark::RHI::SaveCompiledShader(outputPath, result.bytecode)) {
            std::cerr << "ERROR: Failed to write output file: " << outputPath << "\n";
            return 1;
        }

        std::cout << config.inputFile << " -> " << outputPath
                  << " (" << result.bytecode.size() << " bytes, "
                  << elapsed << " ms)\n";
    } else {
        std::cout << config.inputFile << " - OK"
                  << " (" << elapsed << " ms)\n";
    }

    return 0;
}

// ============================================================================
// ARGUMENT PARSING
// ============================================================================

static bool ParseArgs(int argc, char* argv[], CompilerConfig& config)
{
    if (argc < 2) {
        PrintUsage(argv[0]);
        return false;
    }

    bool stageExplicit = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return false;
        }
        else if (arg == "-o" && i + 1 < argc) {
            config.outputFile = argv[++i];
        }
        else if (arg == "-stage" && i + 1 < argc) {
            config.stage = ParseStage(argv[++i]);
            stageExplicit = true;
        }
        else if (arg == "-backend" && i + 1 < argc) {
            config.targetBackend = ParseBackend(argv[++i]);
        }
        else if (arg == "-entry" && i + 1 < argc) {
            config.entryPoint = argv[++i];
        }
        else if (arg.substr(0, 2) == "-D") {
            config.defines.push_back(arg.substr(2));
        }
        else if (arg.substr(0, 2) == "-I") {
            config.includePaths.push_back(arg.substr(2));
        }
        else if (arg == "-O") {
            config.optimization = true;
        }
        else if (arg == "-Od") {
            config.optimization = false;
        }
        else if (arg == "-Zi") {
            config.debugInfo = true;
        }
        else if (arg == "-validate") {
            config.validateOnly = true;
        }
        else if (arg == "-reflect") {
            config.reflect = true;
        }
        else if (arg == "-batch" && i + 1 < argc) {
            config.batchDir = argv[++i];
        }
        else if (arg == "-v") {
            config.verbose = true;
        }
        else if (arg[0] != '-') {
            config.inputFile = arg;
        }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }

    // Infer stage from filename if not explicit
    if (!stageExplicit && !config.inputFile.empty()) {
        config.stage = InferStageFromFilename(config.inputFile);
    }

    if (config.inputFile.empty() && config.batchDir.empty()) {
        std::cerr << "Error: No input file specified.\n";
        PrintUsage(argv[0]);
        return false;
    }

    return true;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[])
{
    CompilerConfig config;

    if (!ParseArgs(argc, argv, config))
        return 1;

    // Single file compilation
    if (!config.inputFile.empty()) {
        return CompileSingleShader(config);
    }

    // Batch mode not yet implemented
    if (!config.batchDir.empty()) {
        std::cerr << "Batch compilation not yet implemented.\n"
                  << "Use compile_shaders.sh / compile_shaders.bat for batch compilation.\n";
        return 1;
    }

    return 0;
}
