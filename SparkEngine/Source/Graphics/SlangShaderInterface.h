/**
 * @file SlangShaderInterface.h
 * @brief Abstract interface for Slang shader compilation and reflection
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a target-agnostic interface to the Slang shader compiler.
 * Supports compilation to DXIL, SPIR-V, MSL, HLSL, CUDA, and WGSL
 * with reflection data extraction for automatic resource binding.
 *
 * Features:
 * - Multi-target compilation (DXIL, SPIRV, MSL, HLSL, CUDA, WGSL)
 * - Optimization levels: None, Default, Maximal
 * - Full shader stage coverage including mesh and amplification shaders
 * - Reflection data: parameters, constant buffers, textures, samplers
 * - Compile from source string or file path
 * - Module interface for multi-entry-point shaders
 *
 * @see ShaderCompilation.h, ShaderCrossCompiler.h, ShaderVariantSystem.h
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Enumerations
    // =========================================================================

    /// @brief Target output format for Slang compilation
    enum class SlangTarget : uint8_t
    {
        DXIL,  ///< DirectX Intermediate Language (SM 6.x)
        SPIRV, ///< Vulkan SPIR-V
        MSL,   ///< Metal Shading Language
        HLSL,  ///< High Level Shading Language source
        CUDA,  ///< NVIDIA CUDA PTX
        WGSL   ///< WebGPU Shading Language
    };

    /// @brief Get display name for a compilation target
    inline const char* SlangTargetName(SlangTarget target)
    {
        switch (target)
        {
        case SlangTarget::DXIL:
            return "DXIL";
        case SlangTarget::SPIRV:
            return "SPIR-V";
        case SlangTarget::MSL:
            return "MSL";
        case SlangTarget::HLSL:
            return "HLSL";
        case SlangTarget::CUDA:
            return "CUDA";
        case SlangTarget::WGSL:
            return "WGSL";
        default:
            return "Unknown";
        }
    }

    /// @brief Optimization level for shader compilation
    enum class SlangOptLevel : uint8_t
    {
        None,    ///< No optimization (fastest compile, debug-friendly)
        Default, ///< Standard optimizations
        Maximal  ///< Aggressive optimizations (slowest compile)
    };

    /// @brief Shader pipeline stage
    enum class SlangShaderStage : uint8_t
    {
        Vertex,
        Fragment,
        Compute,
        Geometry,
        Hull,
        Domain,
        Mesh,
        Amplification
    };

    /// @brief Get display name for a shader stage
    inline const char* SlangShaderStageName(SlangShaderStage stage)
    {
        switch (stage)
        {
        case SlangShaderStage::Vertex:
            return "Vertex";
        case SlangShaderStage::Fragment:
            return "Fragment";
        case SlangShaderStage::Compute:
            return "Compute";
        case SlangShaderStage::Geometry:
            return "Geometry";
        case SlangShaderStage::Hull:
            return "Hull";
        case SlangShaderStage::Domain:
            return "Domain";
        case SlangShaderStage::Mesh:
            return "Mesh";
        case SlangShaderStage::Amplification:
            return "Amplification";
        default:
            return "Unknown";
        }
    }

    // =========================================================================
    // Reflection Data
    // =========================================================================

    /**
     * @brief Describes a single shader parameter binding
     *
     * Captures the name, type, and binding location for a shader resource
     * as reported by Slang's reflection API.
     */
    struct SlangParameterInfo
    {
        std::string name;         ///< Parameter name in shader source
        std::string typeName;     ///< Type as a human-readable string (e.g. "float4", "Texture2D")
        uint32_t bindingSlot = 0; ///< Register or binding index
        uint32_t set = 0;         ///< Descriptor set (Vulkan) or register space (DX12)
        uint32_t offset = 0;      ///< Byte offset within a constant buffer (0 for non-CB params)
        uint32_t size = 0;        ///< Size in bytes (0 if not applicable)
    };

    /**
     * @brief Aggregated reflection data for a compiled shader
     *
     * Groups shader resources by category for easy iteration when
     * setting up pipeline resource layouts.
     */
    struct SlangReflectionData
    {
        std::vector<SlangParameterInfo> parameters; ///< All loose parameters
        std::vector<SlangParameterInfo> cbuffers;   ///< Constant / uniform buffers
        std::vector<SlangParameterInfo> textures;   ///< Texture / image bindings
        std::vector<SlangParameterInfo> samplers;   ///< Sampler state bindings

        /// @brief Total number of reflected resources across all categories
        uint32_t GetTotalBindingCount() const
        {
            return static_cast<uint32_t>(parameters.size() + cbuffers.size() + textures.size() + samplers.size());
        }

        /// @brief Find a parameter by name across all categories, returns nullptr if not found
        const SlangParameterInfo* FindParameter(const std::string& name) const
        {
            for (const auto& p : parameters)
            {
                if (p.name == name)
                    return &p;
            }
            for (const auto& cb : cbuffers)
            {
                if (cb.name == name)
                    return &cb;
            }
            for (const auto& tex : textures)
            {
                if (tex.name == name)
                    return &tex;
            }
            for (const auto& s : samplers)
            {
                if (s.name == name)
                    return &s;
            }
            return nullptr;
        }
    };

    // =========================================================================
    // Compile Request / Result
    // =========================================================================

    /// @brief A single preprocessor define (name and optional value)
    struct SlangDefine
    {
        std::string name;
        std::string value;
    };

    /**
     * @brief Input parameters for a Slang compilation request
     *
     * Specifies the source code (or will be paired with a file path),
     * the entry point, target format, and any preprocessor defines.
     */
    struct SlangCompileRequest
    {
        std::string sourceCode;                            ///< Shader source (empty when compiling from file)
        std::string entryPoint = "main";                   ///< Entry point function name
        SlangShaderStage stage = SlangShaderStage::Vertex; ///< Pipeline stage
        SlangTarget target = SlangTarget::DXIL;            ///< Compilation target
        SlangOptLevel optLevel = SlangOptLevel::Default;   ///< Optimization level
        std::vector<SlangDefine> defines;                  ///< Preprocessor defines
    };

    /**
     * @brief Output of a Slang compilation
     *
     * Contains the compiled bytecode (if successful), any error/warning
     * messages, and reflection data for resource binding setup.
     */
    struct SlangCompileResult
    {
        std::vector<uint8_t> bytecode;  ///< Compiled shader binary (empty on failure)
        std::string errors;             ///< Error and warning messages
        bool success = false;           ///< True if compilation succeeded
        SlangReflectionData reflection; ///< Reflected resource bindings

        /// @brief Check if the result contains warnings even though compilation succeeded
        bool HasWarnings() const { return success && !errors.empty(); }
    };

    // =========================================================================
    // ISlangModule
    // =========================================================================

    /**
     * @brief Interface for a compiled Slang module containing one or more entry points
     *
     * A module represents a single compilation unit that may expose
     * multiple entry points (e.g. vertex + fragment in the same file).
     */
    class ISlangModule
    {
      public:
        virtual ~ISlangModule() = default;

        /// @brief Get the module name (typically the source file stem)
        virtual const std::string& GetName() const = 0;

        /// @brief Get the number of entry points in this module
        virtual uint32_t GetEntryPointCount() const = 0;

        /// @brief Get the name of an entry point by index
        virtual const std::string& GetEntryPoint(uint32_t index) const = 0;

        /// @brief Get aggregated reflection data for the entire module
        virtual const SlangReflectionData& GetReflection() const = 0;
    };

    // =========================================================================
    // ISlangSession
    // =========================================================================

    /**
     * @brief Abstract session interface for Slang shader compilation
     *
     * Manages the lifetime of the Slang compiler context and provides
     * methods to compile shaders from source strings or file paths.
     * Implementations own the underlying Slang global session and
     * any cached compilation state.
     */
    class ISlangSession
    {
      public:
        virtual ~ISlangSession() = default;

        /**
         * @brief Initialize the Slang compiler session
         *
         * Must be called before any compilation. Sets up the global
         * Slang session, configures search paths, and prepares internal state.
         *
         * @return True if initialization succeeded
         */
        virtual bool Initialize() = 0;

        /**
         * @brief Shut down the session and release all resources
         *
         * Invalidates all previously returned modules and compile results.
         * Safe to call multiple times.
         */
        virtual void Shutdown() = 0;

        /**
         * @brief Compile a shader from a source string
         *
         * The request's sourceCode field must be non-empty. The entry point,
         * stage, target, optimization level, and defines are all taken from
         * the request.
         *
         * @param request  Compilation parameters with source code
         * @return Compilation result with bytecode and reflection on success
         */
        virtual SlangCompileResult CompileFromSource(const SlangCompileRequest& request) = 0;

        /**
         * @brief Compile a shader from a file on disk
         *
         * Reads the file at the given path and compiles it. The request's
         * sourceCode field is ignored; entry point, stage, target, and other
         * parameters are taken from the request.
         *
         * @param filePath  Path to the .slang or .hlsl source file
         * @param request   Compilation parameters (sourceCode is ignored)
         * @return Compilation result with bytecode and reflection on success
         */
        virtual SlangCompileResult CompileFromFile(const std::string& filePath, const SlangCompileRequest& request) = 0;
    };

} // namespace Spark::Graphics
