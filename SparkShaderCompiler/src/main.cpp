/**
 * @file main.cpp
 * @brief SparkShaderCompiler - Standalone offline shader compilation tool
 * @author Spark Engine Team
 * @date 2025
 *
 * Compiles HLSL to DirectX bytecode (DXBC, SM 5.0/5.1) through
 * RHIFactory::CompileShader, which wraps d3dcompiler_47 on Windows. Backends
 * whose compiler is not integrated (Vulkan/SPIR-V, OpenGL/GLSL from HLSL) are
 * reported as failures with a non-zero exit status — the tool never writes an
 * output file that is just a copy of its input.
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
#include <cctype>
#include <filesystem>

// Include the RHI shader compilation API
#include "../../SparkEngine/Source/Graphics/RHI/RHIFactory.h"
#include "../../SparkEngine/Source/Graphics/RHI/RHITypes.h"

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
              << "  -stage <stage>   Shader stage: vertex, pixel, geometry, hull, domain, compute,\n"
              << "                   raygen, closesthit, miss, anyhit, intersection, callable\n"
              << "  -backend <api>   Target backend: d3d11, d3d12, vulkan, opengl, auto (default: auto,\n"
              << "                   inferred from the source extension; only d3d11/d3d12 have an\n"
              << "                   integrated compiler)\n"
              << "  -entry <name>    Entry point (default: main)\n"
              << "  -D<DEFINE>       Add preprocessor define\n"
              << "  -I<path>         Add include search path\n"
              << "  -O               Enable optimization (default)\n"
              << "  -Od              Disable optimization\n"
              << "  -Zi              Enable debug info\n"
              << "  -validate        Run the compiler but write nothing; non-zero unless the\n"
              << "                   backend has an integrated compiler and the shader compiles\n"
              << "  -reflect         Print shader reflection data (SPIR-V output only)\n"
              << "  -batch <dir>     Compile all shaders in directory (not with a positional input)\n"
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
    if (str == "vertex" || str == "vert" || str == "vs")
        return Spark::RHI::RHIShaderStage::Vertex;
    if (str == "pixel" || str == "frag" || str == "ps")
        return Spark::RHI::RHIShaderStage::Pixel;
    if (str == "geometry" || str == "geom" || str == "gs")
        return Spark::RHI::RHIShaderStage::Geometry;
    if (str == "hull" || str == "hs")
        return Spark::RHI::RHIShaderStage::Hull;
    if (str == "domain" || str == "ds")
        return Spark::RHI::RHIShaderStage::Domain;
    if (str == "compute" || str == "cs")
        return Spark::RHI::RHIShaderStage::Compute;
    if (str == "raygen" || str == "rgen" || str == "raygeneration")
        return Spark::RHI::RHIShaderStage::RayGeneration;
    if (str == "closesthit" || str == "rchit" || str == "chs")
        return Spark::RHI::RHIShaderStage::ClosestHit;
    if (str == "miss" || str == "rmiss")
        return Spark::RHI::RHIShaderStage::Miss;
    if (str == "anyhit" || str == "rahit")
        return Spark::RHI::RHIShaderStage::AnyHit;
    if (str == "intersection" || str == "rint")
        return Spark::RHI::RHIShaderStage::Intersection;
    if (str == "callable" || str == "rcall")
        return Spark::RHI::RHIShaderStage::Callable;
    std::cerr << "Warning: Unknown shader stage '" << str << "', defaulting to vertex\n";
    return Spark::RHI::RHIShaderStage::Vertex;
}

static Spark::RHI::GraphicsBackend ParseBackend(const std::string& str)
{
    if (str == "d3d11" || str == "dx11")
        return Spark::RHI::GraphicsBackend::D3D11;
    if (str == "d3d12" || str == "dx12")
        return Spark::RHI::GraphicsBackend::D3D12;
    if (str == "vulkan" || str == "vk")
        return Spark::RHI::GraphicsBackend::Vulkan;
    if (str == "opengl" || str == "gl")
        return Spark::RHI::GraphicsBackend::OpenGL;
    if (str == "auto")
        return Spark::RHI::GraphicsBackend::Auto;
    std::cerr << "Warning: Unknown backend '" << str << "', defaulting to auto\n";
    return Spark::RHI::GraphicsBackend::Auto;
}

static const char* StageToString(Spark::RHI::RHIShaderStage stage)
{
    switch (stage)
    {
    case Spark::RHI::RHIShaderStage::Vertex:
        return "Vertex";
    case Spark::RHI::RHIShaderStage::Pixel:
        return "Pixel";
    case Spark::RHI::RHIShaderStage::Geometry:
        return "Geometry";
    case Spark::RHI::RHIShaderStage::Hull:
        return "Hull";
    case Spark::RHI::RHIShaderStage::Domain:
        return "Domain";
    case Spark::RHI::RHIShaderStage::Compute:
        return "Compute";
    case Spark::RHI::RHIShaderStage::RayGeneration:
        return "RayGeneration";
    case Spark::RHI::RHIShaderStage::ClosestHit:
        return "ClosestHit";
    case Spark::RHI::RHIShaderStage::Miss:
        return "Miss";
    case Spark::RHI::RHIShaderStage::AnyHit:
        return "AnyHit";
    case Spark::RHI::RHIShaderStage::Intersection:
        return "Intersection";
    case Spark::RHI::RHIShaderStage::Callable:
        return "Callable";
    default:
        return "Unknown";
    }
}

static std::string InferOutputPath(const std::string& inputFile, Spark::RHI::GraphicsBackend backend)
{
    std::string base = inputFile;
    size_t dotPos = base.find_last_of('.');
    if (dotPos != std::string::npos)
        base = base.substr(0, dotPos);

    const char* ext = Spark::RHI::GetShaderExtension(backend);
    if (backend == Spark::RHI::GraphicsBackend::Vulkan)
        return base + ".spv";
    if (backend == Spark::RHI::GraphicsBackend::D3D11 || backend == Spark::RHI::GraphicsBackend::D3D12)
        return base + ".cso";
    return base + ext;
}

static std::string LowercaseExtension(const std::string& path)
{
    const size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos)
        return {};

    std::string ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext;
}

/// Turn `auto` into the backend implied by the source language, so the output
/// extension and the compiler selection agree. Returns Auto when nothing can be
/// inferred; the caller reports that as an error rather than guessing.
static Spark::RHI::GraphicsBackend ResolveBackend(Spark::RHI::GraphicsBackend requested, const std::string& inputFile)
{
    if (requested != Spark::RHI::GraphicsBackend::Auto)
        return requested;

    const std::string ext = LowercaseExtension(inputFile);
    if (ext == ".hlsl" || ext == ".fx")
    {
#ifdef _WIN32
        return Spark::RHI::GraphicsBackend::D3D11;
#else
        return Spark::RHI::GraphicsBackend::Auto;
#endif
    }
    if (ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".geom" || ext == ".comp")
        return Spark::RHI::GraphicsBackend::OpenGL;

    return Spark::RHI::GraphicsBackend::Auto;
}

/// True when a real compiler stands behind this backend. Only the Direct3D
/// path is wired (d3dcompiler_47); everything else is a passthrough or a stub,
/// which -validate must not report as a pass.
static bool HasIntegratedCompiler(Spark::RHI::GraphicsBackend backend)
{
#ifdef _WIN32
    return backend == Spark::RHI::GraphicsBackend::D3D11 || backend == Spark::RHI::GraphicsBackend::D3D12;
#else
    (void)backend;
    return false;
#endif
}

static Spark::RHI::RHIShaderStage InferStageFromFilename(const std::string& filename)
{
    // Reduce to the bare stem (no directory, no extension) so unrelated path
    // characters cannot influence inference.
    std::string stem = filename;
    size_t slash = stem.find_last_of("/\\");
    if (slash != std::string::npos)
        stem = stem.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos)
        stem = stem.substr(0, dot);

    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    // Final underscore/dash-delimited token (for `blur_vs`-style names).
    std::string lastToken = lower;
    size_t sep = lower.find_last_of("_-");
    if (sep != std::string::npos)
        lastToken = lower.substr(sep + 1);

    // Two-letter stage codes (vs/ps/gs/hs/ds/cs) collide with ordinary words
    // as substrings ("Physics" contains "cs"). Accept them ONLY as an explicit
    // suffix: an uppercase code in the original name (BasicPS) or a whole
    // delimited token (blur_cs).
    auto hasShortCode = [&](const char* lowerCode, const char* upperCode)
    {
        if (lastToken == lowerCode)
            return true;
        std::string upper = upperCode;
        return stem.size() >= upper.size() && stem.compare(stem.size() - upper.size(), upper.size(), upper) == 0;
    };

    // Distinctive full words are safe to match anywhere in the stem.
    if (lower.contains("vert") || hasShortCode("vs", "VS"))
        return Spark::RHI::RHIShaderStage::Vertex;
    if (lower.contains("frag") || lower.contains("pixel") || hasShortCode("ps", "PS"))
        return Spark::RHI::RHIShaderStage::Pixel;
    if (lower.contains("geom") || hasShortCode("gs", "GS"))
        return Spark::RHI::RHIShaderStage::Geometry;
    if (lower.contains("hull") || hasShortCode("hs", "HS"))
        return Spark::RHI::RHIShaderStage::Hull;
    if (lower.contains("domain") || hasShortCode("ds", "DS"))
        return Spark::RHI::RHIShaderStage::Domain;
    if (lower.contains("compute") || hasShortCode("cs", "CS"))
        return Spark::RHI::RHIShaderStage::Compute;
    if (lower.contains("raygen") || lower.contains("rgen"))
        return Spark::RHI::RHIShaderStage::RayGeneration;
    if (lower.contains("closesthit") || lower.contains("rchit"))
        return Spark::RHI::RHIShaderStage::ClosestHit;
    if (lower.contains("miss") || lower.contains("rmiss"))
        return Spark::RHI::RHIShaderStage::Miss;
    if (lower.contains("anyhit") || lower.contains("rahit"))
        return Spark::RHI::RHIShaderStage::AnyHit;
    if (lower.contains("intersection") || lower.contains("rint"))
        return Spark::RHI::RHIShaderStage::Intersection;
    if (lower.contains("callable") || lower.contains("rcall"))
        return Spark::RHI::RHIShaderStage::Callable;

    // Default
    return Spark::RHI::RHIShaderStage::Vertex;
}

// ============================================================================
// COMPILATION
// ============================================================================

static int CompileSingleShader(const CompilerConfig& config)
{
    const Spark::RHI::GraphicsBackend backend = ResolveBackend(config.targetBackend, config.inputFile);
    if (backend == Spark::RHI::GraphicsBackend::Auto)
    {
        std::cerr << "ERROR: Cannot infer a target backend for " << config.inputFile
                  << ": pass -backend d3d11|d3d12|vulkan|opengl\n";
        return 1;
    }

    if (config.validateOnly && !HasIntegratedCompiler(backend))
    {
        std::cout << config.inputFile << " - SKIPPED (no compiler backend integrated for "
                  << Spark::RHI::GetBackendName(backend) << ")\n";
        return 1;
    }

    if (config.verbose)
    {
        std::cout << "Compiling: " << config.inputFile << "\n"
                  << "  Stage:   " << StageToString(config.stage) << "\n"
                  << "  Backend: " << Spark::RHI::GetBackendName(backend) << "\n"
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
    options.targetBackend = backend;
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

    if (!result.success)
    {
        std::cerr << "ERROR: Compilation failed for " << config.inputFile << "\n";
        if (!result.errorMessage.empty())
            std::cerr << "  " << result.errorMessage << "\n";
        return 1;
    }

    if (!result.warningMessage.empty())
    {
        std::cerr << "WARNING: " << result.warningMessage << "\n";
    }

    if (config.verbose)
    {
        std::cout << "  Compiled in " << elapsed << " ms" << " (" << result.bytecode.size() << " bytes)\n";
    }

    // Reflection. ReflectSPIRV only understands SPIR-V; printing its empty result
    // for a DXBC blob would read as "this shader has no bindings".
    const bool isSpirv = result.bytecode.size() >= 4 && result.bytecode[0] == 0x03 && result.bytecode[1] == 0x02 &&
                         result.bytecode[2] == 0x23 && result.bytecode[3] == 0x07;
    if (config.reflect && !isSpirv)
    {
        std::cout << "\nReflection unavailable for " << Spark::RHI::GetBackendName(backend)
                  << " output (SPIR-V only; SPIRV-Reflect is not integrated)\n";
    }
    else if (config.reflect)
    {
        auto reflection = Spark::RHI::ReflectSPIRV(result.bytecode);
        std::cout << "\nShader Reflection:\n";
        std::cout << "  Inputs: " << reflection.inputs.size() << "\n";
        for (const auto& input : reflection.inputs)
        {
            std::cout << "    location=" << input.location << " " << input.semanticName << "\n";
        }
        std::cout << "  Resources: " << reflection.resources.size() << "\n";
        for (const auto& res : reflection.resources)
        {
            std::cout << "    set=" << res.set << " binding=" << res.binding << " " << res.name << " (size=" << res.size
                      << ")\n";
        }
    }

    // Write output
    if (!config.validateOnly)
    {
        std::string outputPath = config.outputFile;
        if (outputPath.empty())
            outputPath = InferOutputPath(config.inputFile, backend);

        std::error_code pathEc;
        if (std::filesystem::equivalent(outputPath, config.inputFile, pathEc))
        {
            std::cerr << "ERROR: Output would overwrite the input file (" << outputPath
                      << "); pass -o to name a distinct output\n";
            return 1;
        }

        if (!Spark::RHI::SaveCompiledShader(outputPath, result.bytecode))
        {
            std::cerr << "ERROR: Failed to write output file: " << outputPath << "\n";
            return 1;
        }

        std::cout << config.inputFile << " -> " << outputPath << " (" << result.bytecode.size() << " bytes, " << elapsed
                  << " ms)\n";
    }
    else
    {
        std::cout << config.inputFile << " - OK" << " (" << elapsed << " ms)\n";
    }

    return 0;
}

// ============================================================================
// ARGUMENT PARSING
// ============================================================================

enum class ParseResult
{
    Success,
    Help,
    Error,
};

static ParseResult ParseArgs(int argc, char* argv[], CompilerConfig& config)
{
    if (argc < 2)
    {
        PrintUsage(argv[0]);
        return ParseResult::Error;
    }

    bool stageExplicit = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help")
        {
            PrintUsage(argv[0]);
            return ParseResult::Help;
        }
        else if (arg == "-o" && i + 1 < argc)
        {
            config.outputFile = argv[++i];
        }
        else if (arg == "-stage" && i + 1 < argc)
        {
            config.stage = ParseStage(argv[++i]);
            stageExplicit = true;
        }
        else if (arg == "-backend" && i + 1 < argc)
        {
            config.targetBackend = ParseBackend(argv[++i]);
        }
        else if (arg == "-entry" && i + 1 < argc)
        {
            config.entryPoint = argv[++i];
        }
        else if (arg.substr(0, 2) == "-D")
        {
            config.defines.push_back(arg.substr(2));
        }
        else if (arg.substr(0, 2) == "-I")
        {
            config.includePaths.push_back(arg.substr(2));
        }
        else if (arg == "-O")
        {
            config.optimization = true;
        }
        else if (arg == "-Od")
        {
            config.optimization = false;
        }
        else if (arg == "-Zi")
        {
            config.debugInfo = true;
        }
        else if (arg == "-validate")
        {
            config.validateOnly = true;
        }
        else if (arg == "-reflect")
        {
            config.reflect = true;
        }
        else if (arg == "-batch" && i + 1 < argc)
        {
            config.batchDir = argv[++i];
        }
        else if (arg == "-v")
        {
            config.verbose = true;
        }
        else if (arg[0] != '-')
        {
            config.inputFile = arg;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << "\n";
            return ParseResult::Error;
        }
    }

    // Infer stage from filename if not explicit
    if (!stageExplicit && !config.inputFile.empty())
    {
        config.stage = InferStageFromFilename(config.inputFile);
    }

    if (config.inputFile.empty() && config.batchDir.empty())
    {
        std::cerr << "Error: No input file specified.\n";
        PrintUsage(argv[0]);
        return ParseResult::Error;
    }

    if (!config.inputFile.empty() && !config.batchDir.empty())
    {
        std::cerr << "Error: -batch cannot be combined with a positional input file\n";
        return ParseResult::Error;
    }

    return ParseResult::Success;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[])
{
    CompilerConfig config;

    const ParseResult parseResult = ParseArgs(argc, argv, config);
    if (parseResult == ParseResult::Help)
        return 0;
    if (parseResult == ParseResult::Error)
        return 1;

    // Single file compilation
    if (!config.inputFile.empty())
    {
        return CompileSingleShader(config);
    }

    // Batch compilation
    if (!config.batchDir.empty())
    {
        namespace fs = std::filesystem;

        if (!fs::exists(config.batchDir) || !fs::is_directory(config.batchDir))
        {
            std::cerr << "Error: Batch directory not found: " << config.batchDir << "\n";
            return 1;
        }

        // Supported shader file extensions
        const std::vector<std::string> shaderExts = {".hlsl", ".glsl",  ".vert",  ".frag",  ".comp", ".geom",
                                                     ".tesc", ".tese",  ".vs",    ".ps",    ".gs",   ".cs",
                                                     ".rgen", ".rmiss", ".rchit", ".rahit", ".rint", ".rcall"};

        auto isShaderFile = [&](const fs::path& path)
        {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return std::find(shaderExts.begin(), shaderExts.end(), ext) != shaderExts.end();
        };

        std::vector<std::string> shaderFiles;
        for (const auto& entry : fs::recursive_directory_iterator(config.batchDir))
        {
            if (entry.is_regular_file() && isShaderFile(entry.path()))
            {
                shaderFiles.push_back(entry.path().string());
            }
        }

        if (shaderFiles.empty())
        {
            std::cerr << "No shader files found in: " << config.batchDir << "\n";
            return 1;
        }

        std::cout << "Batch compiling " << shaderFiles.size() << " shader(s) from " << config.batchDir << "\n";

        // In batch mode -o names an output directory; ensure it exists up front so
        // per-file writes below don't all fail on a missing path.
        if (!config.outputFile.empty())
        {
            std::error_code ec;
            fs::create_directories(config.outputFile, ec);
            if (ec)
            {
                std::cerr << "Error: Could not create output directory '" << config.outputFile << "': " << ec.message()
                          << "\n";
                return 1;
            }
        }

        auto batchStart = std::chrono::high_resolution_clock::now();
        int successCount = 0;
        int failCount = 0;

        for (const auto& shaderPath : shaderFiles)
        {
            CompilerConfig fileConfig = config;
            fileConfig.inputFile = shaderPath;
            fileConfig.batchDir.clear(); // prevent recursion
            fileConfig.stage = InferStageFromFilename(shaderPath);

            const Spark::RHI::GraphicsBackend fileBackend = ResolveBackend(fileConfig.targetBackend, shaderPath);
            if (fileConfig.outputFile.empty())
            {
                fileConfig.outputFile = InferOutputPath(shaderPath, fileBackend);
            }
            else
            {
                // In batch mode with -o, put output files in that directory
                fs::path outDir(config.outputFile);
                fs::path inputName = fs::path(shaderPath).filename();
                std::string outName = InferOutputPath(inputName.string(), fileBackend);
                fileConfig.outputFile = (outDir / outName).string();
            }

            int ret = CompileSingleShader(fileConfig);
            if (ret == 0)
            {
                successCount++;
            }
            else
            {
                failCount++;
            }
        }

        auto batchEnd = std::chrono::high_resolution_clock::now();
        float totalMs = std::chrono::duration<float, std::milli>(batchEnd - batchStart).count();

        std::cout << "\n=== Batch Compilation Summary ===\n"
                  << "  Total:   " << shaderFiles.size() << "\n"
                  << "  Success: " << successCount << "\n"
                  << "  Failed:  " << failCount << "\n"
                  << "  Time:    " << totalMs << " ms\n";

        return failCount > 0 ? 1 : 0;
    }

    return 0;
}
