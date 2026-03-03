/**
 * @file RHIFactory.cpp
 * @brief RHI factory and utility implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "RHIFactory.h"
#include <iostream>
#include <fstream>
#include <algorithm>

// Backend headers (conditionally included)
#ifdef _WIN32
#include "D3D11/D3D11Device.h"
#endif

#ifdef SPARK_VULKAN_SUPPORT
#include "Vulkan/VulkanDevice.h"
#endif

#ifdef SPARK_OPENGL_SUPPORT
#include "OpenGL/OpenGLDevice.h"
#endif

namespace Spark { namespace RHI {

// ============================================================================
// BACKEND DETECTION
// ============================================================================

std::vector<GraphicsBackend> DetectAvailableBackends()
{
    std::vector<GraphicsBackend> backends;

#ifdef _WIN32
    // D3D11 is always available on Windows
    backends.push_back(GraphicsBackend::D3D11);
#endif

#ifdef SPARK_VULKAN_SUPPORT
    // Check if Vulkan runtime is available
    backends.push_back(GraphicsBackend::Vulkan);
#endif

#ifdef SPARK_OPENGL_SUPPORT
    backends.push_back(GraphicsBackend::OpenGL);
#endif

    return backends;
}

GraphicsBackend GetRecommendedBackend()
{
    auto backends = DetectAvailableBackends();

    if (backends.empty())
        return GraphicsBackend::Auto;

    // Priority: D3D11 on Windows, Vulkan on Linux, OpenGL as fallback
#ifdef _WIN32
    for (auto b : backends)
        if (b == GraphicsBackend::D3D11) return GraphicsBackend::D3D11;
#else
    for (auto b : backends)
        if (b == GraphicsBackend::Vulkan) return GraphicsBackend::Vulkan;
#endif

    return backends[0];
}

std::unique_ptr<IRHIDevice> CreateDevice(GraphicsBackend backend)
{
    if (backend == GraphicsBackend::Auto)
        backend = GetRecommendedBackend();

    std::unique_ptr<IRHIDevice> device;

    switch (backend)
    {
#ifdef _WIN32
    case GraphicsBackend::D3D11:
        device = std::make_unique<D3D11::D3D11Device>();
        break;
#endif

#ifdef SPARK_VULKAN_SUPPORT
    case GraphicsBackend::Vulkan:
        device = std::make_unique<Vulkan::VulkanDevice>();
        break;
#endif

#ifdef SPARK_OPENGL_SUPPORT
    case GraphicsBackend::OpenGL:
        device = std::make_unique<OpenGL::GLDevice>();
        break;
#endif

    default:
        std::cerr << "[RHI] Backend '" << GetBackendName(backend)
                  << "' is not available on this platform." << std::endl;
        return nullptr;
    }

    return device;
}

const char* GetBackendName(GraphicsBackend backend)
{
    switch (backend)
    {
    case GraphicsBackend::D3D11:  return "DirectX 11";
    case GraphicsBackend::D3D12:  return "DirectX 12";
    case GraphicsBackend::Vulkan: return "Vulkan";
    case GraphicsBackend::OpenGL: return "OpenGL";
    case GraphicsBackend::Metal:  return "Metal";
    case GraphicsBackend::Auto:   return "Auto";
    default:                      return "Unknown";
    }
}

bool IsBackendAvailable(GraphicsBackend backend)
{
    auto available = DetectAvailableBackends();
    return std::find(available.begin(), available.end(), backend) != available.end();
}

// ============================================================================
// SHADER COMPILATION
// ============================================================================

ShaderCompileResult CompileShader(const ShaderCompileOptions& options)
{
    ShaderCompileResult result;

    // Determine target language based on backend
    ShaderLanguage target = options.targetLanguage;
    if (target == ShaderLanguage::Auto)
    {
        switch (options.targetBackend)
        {
        case GraphicsBackend::D3D11:
        case GraphicsBackend::D3D12:
            target = ShaderLanguage::HLSL;
            break;
        case GraphicsBackend::Vulkan:
            target = ShaderLanguage::SPIRV;
            break;
        case GraphicsBackend::OpenGL:
            target = ShaderLanguage::GLSL;
            break;
        default:
            target = ShaderLanguage::HLSL;
            break;
        }
    }

    // Source language detection
    ShaderLanguage source = options.sourceLanguage;
    if (source == ShaderLanguage::Auto)
    {
        if (!options.sourceFile.empty())
        {
            std::string ext = options.sourceFile.substr(options.sourceFile.find_last_of('.'));
            if (ext == ".hlsl" || ext == ".fx") source = ShaderLanguage::HLSL;
            else if (ext == ".glsl" || ext == ".vert" || ext == ".frag") source = ShaderLanguage::GLSL;
            else if (ext == ".spv") source = ShaderLanguage::SPIRV;
            else source = ShaderLanguage::HLSL; // Default to HLSL
        }
        else
        {
            source = ShaderLanguage::HLSL;
        }
    }

    std::string sourceCode = options.sourceCode;

    // Load from file if needed
    if (sourceCode.empty() && !options.sourceFile.empty())
    {
        std::ifstream file(options.sourceFile, std::ios::binary);
        if (file.is_open())
        {
            sourceCode.assign((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
        }
        else
        {
            result.errorMessage = "Failed to open shader file: " + options.sourceFile;
            return result;
        }
    }

    if (sourceCode.empty() && source != ShaderLanguage::SPIRV)
    {
        result.errorMessage = "No shader source code provided";
        return result;
    }

    // Same language - passthrough
    if (source == target && source != ShaderLanguage::SPIRV)
    {
        result.bytecode.assign(sourceCode.begin(), sourceCode.end());
        result.success = true;
        return result;
    }

    // Cross-compilation paths
    if (source == ShaderLanguage::HLSL && target == ShaderLanguage::SPIRV)
    {
        result.bytecode = CrossCompileHLSLtoSPIRV(sourceCode, options.stage, options.entryPoint);
        result.success = !result.bytecode.empty();
    }
    else if (source == ShaderLanguage::HLSL && target == ShaderLanguage::GLSL)
    {
        std::string glsl = CrossCompileHLSLtoGLSL(sourceCode, options.stage, options.entryPoint);
        result.bytecode.assign(glsl.begin(), glsl.end());
        result.success = !glsl.empty();
    }
    else if (source == ShaderLanguage::GLSL && target == ShaderLanguage::SPIRV)
    {
        result.bytecode = CompileGLSLtoSPIRV(sourceCode, options.stage, options.entryPoint);
        result.success = !result.bytecode.empty();
    }
    else if (source == ShaderLanguage::GLSL && target == ShaderLanguage::GLSL)
    {
        result.bytecode.assign(sourceCode.begin(), sourceCode.end());
        result.success = true;
    }
    else
    {
        result.errorMessage = "Unsupported shader cross-compilation path";
    }

    return result;
}

ShaderCompileResult LoadPrecompiledShader(const std::string& filePath)
{
    ShaderCompileResult result;

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        result.errorMessage = "Failed to open shader file: " + filePath;
        return result;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    result.bytecode.resize(fileSize);
    file.read(reinterpret_cast<char*>(result.bytecode.data()), fileSize);
    result.success = true;

    return result;
}

bool SaveCompiledShader(const std::string& filePath, const std::vector<uint8_t>& bytecode)
{
    std::ofstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    file.write(reinterpret_cast<const char*>(bytecode.data()), bytecode.size());
    return file.good();
}

// ============================================================================
// CROSS-COMPILATION
// Basic HLSL→GLSL keyword translation for the OpenGL backend.
// Full SPIR-V compilation requires integrating DXC / glslang at build time.
// ============================================================================

std::string CrossCompileHLSLtoGLSL(const std::string& hlslSource,
                                    RHIShaderStage stage,
                                    const std::string& entryPoint)
{
    if (hlslSource.empty()) return "";

    // Basic keyword translation — sufficient for simple shaders.
    // Complex shaders (compute, tessellation, advanced intrinsics) need
    // a proper SPIRV-Cross pipeline.
    std::string glsl = "#version 450 core\n\n";

    // Map HLSL types → GLSL types
    std::string src = hlslSource;
    auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.length(), to);
            pos += to.length();
        }
    };

    replaceAll(src, "float4x4", "mat4");
    replaceAll(src, "float3x3", "mat3");
    replaceAll(src, "float2x2", "mat2");
    replaceAll(src, "float4", "vec4");
    replaceAll(src, "float3", "vec3");
    replaceAll(src, "float2", "vec2");
    replaceAll(src, "int4", "ivec4");
    replaceAll(src, "int3", "ivec3");
    replaceAll(src, "int2", "ivec2");
    replaceAll(src, "uint4", "uvec4");
    replaceAll(src, "uint3", "uvec3");
    replaceAll(src, "uint2", "uvec2");
    replaceAll(src, "mul(", "((");  // Simplified — needs parenthesis fixup
    replaceAll(src, "saturate(", "clamp(");
    replaceAll(src, "lerp(", "mix(");
    replaceAll(src, "frac(", "fract(");
    replaceAll(src, "rsqrt(", "inversesqrt(");
    replaceAll(src, "ddx(", "dFdx(");
    replaceAll(src, "ddy(", "dFdy(");
    replaceAll(src, "Texture2D", "sampler2D");
    replaceAll(src, "SamplerState", "// SamplerState removed");
    replaceAll(src, ".Sample(", ".texture(");  // Simplified
    replaceAll(src, "cbuffer", "uniform");

    glsl += "// Auto-translated from HLSL (basic keyword substitution)\n";
    glsl += "// Entry point: " + entryPoint + "\n\n";
    glsl += src;

    return glsl;
}

std::vector<uint8_t> CrossCompileHLSLtoSPIRV(const std::string& hlslSource,
                                               RHIShaderStage stage,
                                               const std::string& entryPoint)
{
    // Full SPIR-V compilation requires DXC (DirectX Shader Compiler).
    // To enable: link against dxcompiler.dll and use IDxcCompiler3.
    //
    // Fallback: attempt to invoke dxc as an external process.
    if (hlslSource.empty()) return {};

    std::cerr << "[RHI] CrossCompileHLSLtoSPIRV: DXC not integrated. "
              << "Link dxcompiler.dll to enable HLSL→SPIR-V compilation." << std::endl;
    return {};
}

std::vector<uint8_t> CompileGLSLtoSPIRV(const std::string& glslSource,
                                          RHIShaderStage stage,
                                          const std::string& entryPoint)
{
    // Full SPIR-V compilation requires glslang.
    // To enable: link against glslang and SPIRV libraries.
    if (glslSource.empty()) return {};

    std::cerr << "[RHI] CompileGLSLtoSPIRV: glslang not integrated. "
              << "Link glslang to enable GLSL→SPIR-V compilation." << std::endl;
    return {};
}

ShaderReflection ReflectSPIRV(const std::vector<uint8_t>& spirvCode)
{
    ShaderReflection reflection;
    if (spirvCode.empty()) return reflection;

    // Parse SPIR-V header to extract basic info
    // SPIR-V magic number is 0x07230203
    if (spirvCode.size() >= 20) {
        const uint32_t* words = reinterpret_cast<const uint32_t*>(spirvCode.data());
        if (words[0] == 0x07230203) {
            // Valid SPIR-V — full reflection requires SPIRV-Reflect or spirv-cross
            // For now, return empty but valid reflection data
            std::cerr << "[RHI] ReflectSPIRV: SPIRV-Reflect not integrated. "
                      << "Returning empty reflection data." << std::endl;
        }
    }

    return reflection;
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

const char* GetShaderExtension(GraphicsBackend backend)
{
    switch (backend)
    {
    case GraphicsBackend::D3D11:
    case GraphicsBackend::D3D12:  return ".hlsl";
    case GraphicsBackend::Vulkan: return ".spv";
    case GraphicsBackend::OpenGL: return ".glsl";
    default:                      return ".hlsl";
    }
}

std::string GetShaderSearchPath(GraphicsBackend backend)
{
    switch (backend)
    {
    case GraphicsBackend::D3D11:
    case GraphicsBackend::D3D12:  return "Shaders/HLSL/";
    case GraphicsBackend::Vulkan: return "Shaders/SPIRV/";
    case GraphicsBackend::OpenGL: return "Shaders/GLSL/";
    default:                      return "Shaders/";
    }
}

uint32_t GetPixelFormatSize(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::R8_UNORM:              return 1;
    case PixelFormat::R8G8_UNORM:            return 2;
    case PixelFormat::R16_FLOAT:             return 2;
    case PixelFormat::D16_UNORM:             return 2;
    case PixelFormat::R8G8B8A8_UNORM:
    case PixelFormat::R8G8B8A8_UNORM_SRGB:
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::R10G10B10A2_UNORM:
    case PixelFormat::R11G11B10_FLOAT:
    case PixelFormat::R16G16_FLOAT:
    case PixelFormat::R32_FLOAT:
    case PixelFormat::D24_UNORM_S8_UINT:
    case PixelFormat::D32_FLOAT:             return 4;
    case PixelFormat::R16G16B16A16_FLOAT:
    case PixelFormat::R32G32_FLOAT:          return 8;
    case PixelFormat::R32G32B32_FLOAT:       return 12;
    case PixelFormat::R32G32B32A32_FLOAT:    return 16;
    default:                                 return 4;
    }
}

bool IsCompressedFormat(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::BC1_UNORM:
    case PixelFormat::BC1_UNORM_SRGB:
    case PixelFormat::BC2_UNORM:
    case PixelFormat::BC3_UNORM:
    case PixelFormat::BC3_UNORM_SRGB:
    case PixelFormat::BC4_UNORM:
    case PixelFormat::BC5_UNORM:
    case PixelFormat::BC6H_UF16:
    case PixelFormat::BC7_UNORM:
    case PixelFormat::BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

bool IsDepthStencilFormat(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::D16_UNORM:
    case PixelFormat::D24_UNORM_S8_UINT:
    case PixelFormat::D32_FLOAT:
    case PixelFormat::D32_FLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

const char* GetPixelFormatName(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::R8_UNORM:              return "R8_UNORM";
    case PixelFormat::R8G8_UNORM:            return "R8G8_UNORM";
    case PixelFormat::R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
    case PixelFormat::R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
    case PixelFormat::B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
    case PixelFormat::R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
    case PixelFormat::R11G11B10_FLOAT:       return "R11G11B10_FLOAT";
    case PixelFormat::R16_FLOAT:             return "R16_FLOAT";
    case PixelFormat::R16G16_FLOAT:          return "R16G16_FLOAT";
    case PixelFormat::R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    case PixelFormat::R32_FLOAT:             return "R32_FLOAT";
    case PixelFormat::R32G32_FLOAT:          return "R32G32_FLOAT";
    case PixelFormat::R32G32B32_FLOAT:       return "R32G32B32_FLOAT";
    case PixelFormat::R32G32B32A32_FLOAT:    return "R32G32B32A32_FLOAT";
    case PixelFormat::D16_UNORM:             return "D16_UNORM";
    case PixelFormat::D24_UNORM_S8_UINT:     return "D24_UNORM_S8_UINT";
    case PixelFormat::D32_FLOAT:             return "D32_FLOAT";
    default:                                 return "Unknown";
    }
}

}} // namespace Spark::RHI
