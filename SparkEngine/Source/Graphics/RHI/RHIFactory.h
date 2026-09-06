/**
 * @file RHIFactory.h
 * @brief Factory and utility functions for RHI backend selection
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides runtime graphics API selection, backend detection,
 * and cross-platform shader compilation utilities.
 */

#pragma once

#include "RHIDevice.h"
#include "RHITypes.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace Spark
{
    namespace RHI
    {

        // ============================================================================
        // BACKEND DETECTION & CREATION
        // ============================================================================

        /**
 * @brief Detect and return all available graphics backends on the system
 */
        std::vector<GraphicsBackend> DetectAvailableBackends();

        /**
 * @brief Get the recommended backend for the current platform
 */
        GraphicsBackend GetRecommendedBackend();

        /**
 * @brief Create an RHI device for the specified backend
 * @param backend Graphics API to use (Auto will pick the best available)
 * @return Unique pointer to the created device, or nullptr on failure
 */
        std::unique_ptr<IRHIDevice> CreateDevice(GraphicsBackend backend = GraphicsBackend::Auto);

        /**
 * @brief Get a human-readable name for a graphics backend
 */
        const char* GetBackendName(GraphicsBackend backend);

        /**
 * @brief Check if a specific backend is available on the current system
 */
        bool IsBackendAvailable(GraphicsBackend backend);

        // ============================================================================
        // CROSS-PLATFORM SHADER COMPILATION
        // ============================================================================

        /**
 * @brief Shader compilation result
 */
        struct ShaderCompileResult
        {
            bool success = false;
            std::vector<uint8_t> bytecode;
            std::string errorMessage;
            std::string warningMessage;
            std::string infoLog;
        };

        /**
 * @brief Cross-platform shader compilation options
 */
        struct ShaderCompileOptions
        {
            RHIShaderStage stage = RHIShaderStage::Vertex;
            ShaderLanguage sourceLanguage = ShaderLanguage::Auto;
            ShaderLanguage targetLanguage = ShaderLanguage::Auto;
            GraphicsBackend targetBackend = GraphicsBackend::Auto;
            std::string entryPoint = "main";
            std::string sourceFile;
            std::string sourceCode;
            std::vector<std::string> defines;
            std::vector<std::string> includePaths;
            bool optimizationEnabled = true;
            bool debugInfoEnabled = false;
            bool generateReflection = false;
        };

        /**
 * @brief Compile a shader for a specific backend
 *
 * Implemented paths:
 * - HLSL -> DXBC (D3D11/D3D12) via d3dcompiler_47, Windows only, SM 5.0/5.1
 * - GLSL -> GLSL (OpenGL): passthrough, because the driver compiles source text
 *
 * Every other path fails with an explicit "not integrated" errorMessage:
 * HLSL -> SPIR-V needs DXC, GLSL -> SPIR-V needs glslang, HLSL -> GLSL needs
 * SPIRV-Cross, and ray tracing stages need DXC's lib_6_3 profile. The function
 * never reports success for bytes no compiler has parsed.
 */
        ShaderCompileResult CompileShader(const ShaderCompileOptions& options);

        /**
 * @brief Load pre-compiled shader bytecode from file
 */
        ShaderCompileResult LoadPrecompiledShader(const std::string& filePath);

        /**
 * @brief Save compiled shader bytecode to file
 */
        bool SaveCompiledShader(const std::string& filePath, const std::vector<uint8_t>& bytecode);

        // ============================================================================
        // SHADER CROSS-COMPILATION PIPELINE
        // ============================================================================

        /**
 * @brief Keyword-substitution HLSL->GLSL shim. NOT a cross-compiler.
 *
 * Translates a handful of types, intrinsics and semantics textually. The output
 * is a readable starting point, not compilable GLSL, so CompileShader does not
 * use it — an HLSL->GLSL request fails with a "requires SPIRV-Cross" message.
 */
        std::string CrossCompileHLSLtoGLSL(const std::string& hlslSource, RHIShaderStage stage,
                                           const std::string& entryPoint = "main");

        /**
 * @brief Cross-compile HLSL to SPIR-V. Not integrated: always returns empty.
 */
        std::vector<uint8_t> CrossCompileHLSLtoSPIRV(const std::string& hlslSource, RHIShaderStage stage,
                                                     const std::string& entryPoint = "main");

        /**
 * @brief Compile GLSL to SPIR-V using glslang. Not integrated: always returns empty.
 */
        std::vector<uint8_t> CompileGLSLtoSPIRV(const std::string& glslSource, RHIShaderStage stage,
                                                const std::string& entryPoint = "main");

        // ============================================================================
        // SHADER REFLECTION
        // ============================================================================

        /**
 * @brief Reflected shader resource binding
 */
        struct ShaderResourceBinding
        {
            std::string name;
            uint32_t binding = 0;
            uint32_t set = 0;
            uint32_t size = 0;
            enum class Type
            {
                ConstantBuffer,
                Texture,
                Sampler,
                StorageBuffer,
                StorageImage
            } type = Type::ConstantBuffer;
        };

        /**
 * @brief Reflected vertex input attribute
 */
        struct ShaderInputAttribute
        {
            std::string semanticName;
            uint32_t location = 0;
            RHIVertexFormat format = RHIVertexFormat::Float3;
        };

        /**
 * @brief Complete shader reflection data
 */
        struct ShaderReflection
        {
            std::vector<ShaderResourceBinding> resources;
            std::vector<ShaderInputAttribute> inputs;
            uint32_t threadGroupSizeX = 0;
            uint32_t threadGroupSizeY = 0;
            uint32_t threadGroupSizeZ = 0;
        };

        /**
 * @brief Reflect SPIR-V shader to get binding information.
 *
 * SPIRV-Reflect is not integrated: this returns an empty reflection for every
 * input. Callers must not present an empty result as "the shader has no
 * bindings" — check the SPIR-V magic (0x07230203) before reporting.
 */
        ShaderReflection ReflectSPIRV(const std::vector<uint8_t>& spirvCode);

        // ============================================================================
        // UTILITY FUNCTIONS
        // ============================================================================

        /**
 * @brief Get the appropriate shader file extension for a backend
 */
        const char* GetShaderExtension(GraphicsBackend backend);

        /**
 * @brief Get the shader search path for a backend
 */
        std::string GetShaderSearchPath(GraphicsBackend backend);

        /**
 * @brief Get a human-readable name for a pixel format
 */
        const char* GetPixelFormatName(PixelFormat format);

    } // namespace RHI
} // namespace Spark

// Format utility functions (GetFormatSize, IsCompressedFormat, etc.) are in RHIFormatUtils.h
#include "RHIFormatUtils.h"
