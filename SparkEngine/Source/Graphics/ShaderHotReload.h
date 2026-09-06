/**
 * @file ShaderHotReload.h
 * @brief Shader file watcher and hot-reload system
 * @author Spark Engine Team
 * @date 2026
 *
 * Monitors shader source directories for file changes and automatically
 * recompiles modified shaders at runtime. Supports vertex, pixel, geometry,
 * compute, hull, and domain shader types with configurable poll intervals.
 *
 * ## Usage
 * @code
 *   auto& hotReload = Spark::Graphics::ShaderHotReload::GetInstance();
 *   hotReload.Initialize("Assets/Shaders");
 *   hotReload.OnShaderReloaded([](const auto& event) {
 *       if (!event.success)
 *           LOG_ERROR("Shader {} failed: {}", event.shaderName, event.errorMessage);
 *   });
 *
 *   // In main loop:
 *   hotReload.Update(deltaTime);
 * @endcode
 *
 * @see ScriptHotReload.h (similar pattern for AngelScript files)
 */

#pragma once

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "RHI/RHIFactory.h"
#include "Utils/SparkConsole.h"

namespace Spark::Graphics
{

    // ============================================================================
    // Shader Types
    // ============================================================================

    /** @brief Shader stage type for classification during hot-reload. */
    enum class ShaderStageType : uint8_t
    {
        Vertex,
        Pixel,
        Geometry,
        Compute,
        Hull,
        Domain
    };

    // ============================================================================
    // ShaderFileInfo — metadata for a watched shader file
    // ============================================================================

    /** @brief Tracked metadata for a single shader source file. */
    struct ShaderFileInfo
    {
        std::string path;                                     ///< Absolute path to the shader source file.
        uint64_t lastWriteTime{};                             ///< Last write time as filesystem ticks.
        ShaderStageType shaderType = ShaderStageType::Vertex; ///< Stage classification.
        std::string shaderName;                               ///< Logical shader name (typically filename stem).
    };

    // ============================================================================
    // ShaderReloadEvent — passed to reload callbacks
    // ============================================================================

    /** @brief Describes the result of a shader reload attempt. */
    struct ShaderReloadEvent
    {
        std::string shaderName;            ///< Name of the reloaded shader.
        std::string shaderPath;            ///< File path of the shader source.
        bool success = false;              ///< True if compilation succeeded.
        std::string errorMessage;          ///< Compiler error text (empty on success).
        std::string compileLog;            ///< Compiler log text (warnings/errors).
        float compilationTimeMs{};         ///< Time spent compiling in milliseconds.
        bool reusedPreviousBinary = false; ///< True when compile failed and previous shader stays active.
    };

    // ============================================================================
    // ShaderHotReload — singleton file watcher + recompiler
    // ============================================================================

    /**
     * @class ShaderHotReload
     * @brief Singleton that watches shader directories and recompiles on change.
     *
     * Call Update() each frame; the system accumulates time and polls the
     * filesystem at the configured interval. When a shader file's write time
     * changes, it is recompiled and registered callbacks are notified.
     */
    class ShaderHotReload
    {
      public:
        /** @brief Get the singleton instance. */
        static ShaderHotReload& GetInstance()
        {
            static ShaderHotReload s;
            return s;
        }

        // -- Lifecycle --

        /**
         * @brief Initialize with a root shader directory.
         * @param shaderDirectory Root path to scan for shader files.
         */
        void Initialize(std::string_view shaderDirectory)
        {
            m_watchDirectories.clear();
            m_watchDirectorySet.clear();
            m_watchedFiles.clear();
            {
                std::scoped_lock lock(m_compiledShaderMutex);
                m_compiledShaders.clear();
                m_shaderSwapGenerations.clear();
            }
            m_reloadCount = 0;
            m_timeSinceLastPoll = 0.0f;
            m_enabled = true;
            AddWatchDirectory(shaderDirectory);
        }

        /** @brief Shut down and clear all watched state. */
        void Shutdown()
        {
            m_watchedFiles.clear();
            m_watchDirectories.clear();
            m_watchDirectorySet.clear();
            m_reloadCallbacks.clear();
            {
                std::scoped_lock lock(m_compiledShaderMutex);
                m_compiledShaders.clear();
                m_shaderSwapGenerations.clear();
            }
            m_enabled = false;
            m_reloadCount = 0;
        }

        /**
         * @brief Per-frame update — accumulates time and polls when interval elapses.
         * @param dt Delta time in seconds.
         */
        void Update(float dt)
        {
            if (!m_enabled)
                return;

            m_timeSinceLastPoll += dt;
            if (m_timeSinceLastPoll >= m_pollInterval)
            {
                m_timeSinceLastPoll = 0.0f;
                CheckForChanges();
            }
        }

        // -- Configuration --

        /**
         * @brief Set the interval between filesystem polls.
         * @param seconds Seconds between polls (clamped to >= 0.05).
         */
        void SetPollInterval(float seconds) { m_pollInterval = (seconds < 0.05f) ? 0.05f : seconds; }

        /** @brief Get the current poll interval in seconds. */
        [[nodiscard]] float GetPollInterval() const { return m_pollInterval; }

        /**
         * @brief Add an additional directory to watch.
         *
         * Directories are de-duplicated by canonical path: every shader loader
         * registers its parent directory, so N shaders from one folder must not
         * trigger N recursive scans or N watch entries.
         * @param dir Directory path to add.
         */
        void AddWatchDirectory(std::string_view dir)
        {
            if (!m_watchDirectorySet.insert(CanonicalWatchKey(dir)).second)
                return;
            m_watchDirectories.emplace_back(dir);
            ScanDirectory(m_watchDirectories.back());
        }

        /**
         * @brief Remove a watched directory.
         * @param dir Directory path to remove.
         */
        void RemoveWatchDirectory(std::string_view dir)
        {
            std::string target(dir);
            std::erase(m_watchDirectories, target);
            m_watchDirectorySet.erase(CanonicalWatchKey(dir));
        }

        // -- Actions --

        /** @brief Recompile every watched shader regardless of file changes. */
        void ForceReloadAll()
        {
            for (auto& [name, info] : m_watchedFiles)
            {
                CompileShader(info);
            }
        }

        /**
         * @brief Force recompile a specific shader by name.
         * @param shaderName Logical name of the shader to reload.
         */
        void ForceReload(std::string_view shaderName)
        {
            std::string key(shaderName);
            if (auto it = m_watchedFiles.find(key); it != m_watchedFiles.end())
            {
                CompileShader(it->second);
            }
        }

        // -- Queries --

        /** @brief Number of shader files currently being watched. */
        [[nodiscard]] uint32_t GetWatchedShaderCount() const { return static_cast<uint32_t>(m_watchedFiles.size()); }

        /** @brief Total number of successful reloads since initialization. */
        [[nodiscard]] uint32_t GetReloadCount() const { return m_reloadCount; }

        /** @brief Returns true if a compiled binary is currently available for shaderName. */
        [[nodiscard]] bool HasCompiledShader(std::string_view shaderName) const
        {
            std::scoped_lock lock(m_compiledShaderMutex);
            return m_compiledShaders.contains(std::string(shaderName));
        }

        /**
         * @brief Copy of the compiled blob stored for shaderName (empty when none).
         *
         * Lets callers and tests assert that a successful reload stored real
         * bytecode (DXBC on Windows) rather than source text.
         */
        [[nodiscard]] std::vector<uint8_t> GetCompiledShader(std::string_view shaderName) const
        {
            std::scoped_lock lock(m_compiledShaderMutex);
            if (auto it = m_compiledShaders.find(std::string(shaderName)); it != m_compiledShaders.end())
                return it->second;
            return {};
        }

        /** @brief Returns monotonically increasing successful swap count for shaderName. */
        [[nodiscard]] uint64_t GetShaderSwapGeneration(std::string_view shaderName) const
        {
            std::scoped_lock lock(m_compiledShaderMutex);
            if (auto it = m_shaderSwapGenerations.find(std::string(shaderName)); it != m_shaderSwapGenerations.end())
                return it->second;
            return 0;
        }

        /** @brief True if currently watching (initialized and enabled). */
        [[nodiscard]] bool IsWatching() const { return m_enabled && !m_watchDirectories.empty(); }

        /** @brief Enable or disable the hot-reload polling. */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        /** @brief Check if hot-reload is enabled. */
        [[nodiscard]] bool IsEnabled() const { return m_enabled; }

        /**
         * @brief Graphics backend reloads are compiled for.
         *
         * Auto (the default) lets RHI::CompileShader infer the target from the
         * source language. The graphics engine sets the running backend at init so
         * the HLSL profile matches the device (D3D11 -> SM 5.0, D3D12 -> SM 5.1).
         */
        void SetTargetBackend(Spark::RHI::GraphicsBackend backend) { m_targetBackend = backend; }

        /** @brief Backend passed to RHI::CompileShader for reloads. */
        [[nodiscard]] Spark::RHI::GraphicsBackend GetTargetBackend() const { return m_targetBackend; }

        // -- Callbacks --

        /**
         * @brief Register a callback invoked after each shader reload attempt.
         * @param callback Function receiving a ShaderReloadEvent.
         */
        void OnShaderReloaded(std::function<void(const ShaderReloadEvent&)> callback)
        {
            m_reloadCallbacks.push_back(std::move(callback));
        }

        // -- Console --

        /** @brief Return a status string for the engine console. */
        [[nodiscard]] std::string Console_GetStatus() const
        {
            return "ShaderHotReload: " + std::string(m_enabled ? "enabled" : "disabled") +
                   " | watched=" + std::to_string(GetWatchedShaderCount()) +
                   " | dirs=" + std::to_string(m_watchDirectories.size()) +
                   " | reloads=" + std::to_string(m_reloadCount) + " | interval=" + std::to_string(m_pollInterval) +
                   "s";
        }

      private:
        ShaderHotReload() = default;
        ~ShaderHotReload() = default;
        ShaderHotReload(const ShaderHotReload&) = delete;
        ShaderHotReload& operator=(const ShaderHotReload&) = delete;

        /** @brief Classify a shader file by its extension. */
        static ShaderStageType ClassifyShader(const std::filesystem::path& filePath)
        {
            auto ext = filePath.extension().string();
            if (ext == ".vs" || ext == ".vert")
                return ShaderStageType::Vertex;
            if (ext == ".ps" || ext == ".frag")
                return ShaderStageType::Pixel;
            if (ext == ".gs" || ext == ".geom")
                return ShaderStageType::Geometry;
            if (ext == ".cs" || ext == ".comp")
                return ShaderStageType::Compute;
            if (ext == ".hs" || ext == ".hull")
                return ShaderStageType::Hull;
            if (ext == ".ds" || ext == ".domn")
                return ShaderStageType::Domain;

            // HLSL convention: inspect filename suffix before extension
            auto stem = filePath.stem().string();
            if (stem.ends_with("_VS"))
                return ShaderStageType::Vertex;
            if (stem.ends_with("_PS"))
                return ShaderStageType::Pixel;
            if (stem.ends_with("_GS"))
                return ShaderStageType::Geometry;
            if (stem.ends_with("_CS"))
                return ShaderStageType::Compute;
            if (stem.ends_with("_HS"))
                return ShaderStageType::Hull;
            if (stem.ends_with("_DS"))
                return ShaderStageType::Domain;

            return ShaderStageType::Vertex; // Default fallback
        }

        /** @brief Return true if the file extension looks like a shader. */
        static bool IsShaderFile(const std::filesystem::path& filePath)
        {
            static const std::vector<std::string> shaderExtensions = {".hlsl", ".glsl", ".vs",   ".ps",   ".gs",
                                                                      ".cs",   ".hs",   ".ds",   ".vert", ".frag",
                                                                      ".geom", ".comp", ".hull", ".domn"};
            auto ext = filePath.extension().string();
            for (const auto& se : shaderExtensions)
            {
                if (ext == se)
                    return true;
            }
            return false;
        }

        static Spark::RHI::RHIShaderStage ToRHIStage(ShaderStageType stage)
        {
            switch (stage)
            {
            case ShaderStageType::Vertex:
                return Spark::RHI::RHIShaderStage::Vertex;
            case ShaderStageType::Pixel:
                return Spark::RHI::RHIShaderStage::Pixel;
            case ShaderStageType::Geometry:
                return Spark::RHI::RHIShaderStage::Geometry;
            case ShaderStageType::Compute:
                return Spark::RHI::RHIShaderStage::Compute;
            case ShaderStageType::Hull:
                return Spark::RHI::RHIShaderStage::Hull;
            case ShaderStageType::Domain:
                return Spark::RHI::RHIShaderStage::Domain;
            }

            return Spark::RHI::RHIShaderStage::Vertex;
        }

        static Spark::RHI::ShaderLanguage DetermineSourceLanguage(const std::filesystem::path& filePath)
        {
            const std::string ext = filePath.extension().string();
            if (ext == ".hlsl" || ext == ".vs" || ext == ".ps" || ext == ".gs" || ext == ".cs" || ext == ".hs" ||
                ext == ".ds")
            {
                return Spark::RHI::ShaderLanguage::HLSL;
            }

            if (ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".geom" || ext == ".comp")
                return Spark::RHI::ShaderLanguage::GLSL;

            return Spark::RHI::ShaderLanguage::Auto;
        }

        /** @brief Canonical de-duplication key for a watch directory. */
        static std::string CanonicalWatchKey(std::string_view dir)
        {
            std::error_code ec;
            const auto canon = std::filesystem::weakly_canonical(std::filesystem::path(dir), ec);
            return ec ? std::string(dir) : canon.string();
        }

        /// Directory names never descended into during a scan: user data, temp
        /// output and build trees can be huge and contain no engine shaders.
        static bool IsDeniedScanDirectory(const std::filesystem::path& dirName)
        {
            const std::string name = dirName.string();
            return name == "Saves" || name == "Temp" || name == "LivePackages" || name.rfind("build", 0) == 0;
        }

        /** @brief Scan a directory (bounded depth, deny-listed subtrees skipped) for shader files. */
        void ScanDirectory(const std::string& directory)
        {
            std::error_code ec;
            if (!std::filesystem::exists(directory, ec))
                return;

            constexpr int kMaxScanDepth = 6;
            auto it = std::filesystem::recursive_directory_iterator(directory, ec);
            const std::filesystem::recursive_directory_iterator end;
            if (ec)
            {
                Spark::SimpleConsole::GetInstance().LogWarning("Shader scan could not open '" + directory +
                                                               "': " + ec.message());
                return;
            }

            while (it != end)
            {
                // Per-entry queries use their own error_code. Sharing one with the
                // loop condition made a single unreadable entry (permission denied,
                // broken reparse point) end the whole walk, silently truncating the
                // watch list with no diagnostic. A bad entry is skipped instead.
                std::error_code entryEc;
                const std::filesystem::path entryPath = it->path();

                if (it->is_directory(entryEc))
                {
                    if (it.depth() >= kMaxScanDepth || IsDeniedScanDirectory(entryPath.filename()))
                    {
                        it.disable_recursion_pending();
                    }
                    else
                    {
                        // Probe before descending. recursive_directory_iterator reports an
                        // unreadable subtree from increment(), and implementations answer
                        // that by moving to the end iterator — which would drop every
                        // shader after it. Skipping the subtree keeps the scan going.
                        std::error_code probeEc;
                        (void)std::filesystem::directory_iterator(entryPath, probeEc);
                        if (probeEc)
                        {
                            Spark::SimpleConsole::GetInstance().LogWarning(
                                "Shader scan skipped unreadable directory '" + entryPath.string() +
                                "': " + probeEc.message());
                            it.disable_recursion_pending();
                        }
                    }
                }
                else if (!entryEc && it->is_regular_file(entryEc) && IsShaderFile(entryPath))
                {
                    std::error_code timeEc;
                    const auto ftime = std::filesystem::last_write_time(entryPath, timeEc);
                    if (timeEc)
                    {
                        // No baseline timestamp means CheckForChanges could not tell a
                        // change from a stat failure; leave the file unwatched and say so.
                        Spark::SimpleConsole::GetInstance().LogWarning("Shader scan skipped '" + entryPath.string() +
                                                                       "': " + timeEc.message());
                    }
                    else
                    {
                        ShaderFileInfo info;
                        info.path = entryPath.string();
                        info.shaderName = entryPath.stem().string();
                        info.shaderType = ClassifyShader(entryPath);
                        info.lastWriteTime = static_cast<uint64_t>(ftime.time_since_epoch().count());

                        m_watchedFiles[info.shaderName] = std::move(info);
                    }
                }

                it.increment(ec);
                if (ec)
                {
                    // Report the subtree we could not walk rather than treating the
                    // failure as end-of-scan. The iterator either advanced past the
                    // bad entry (loop continues) or reached the end (loop exits).
                    Spark::SimpleConsole::GetInstance().LogWarning("Shader scan error under '" + directory +
                                                                   "': " + ec.message());
                    ec.clear();
                }
            }
        }

        /** @brief Check all watched files for timestamp changes and recompile. */
        void CheckForChanges()
        {
            std::error_code ec;
            for (auto& [name, info] : m_watchedFiles)
            {
                if (!std::filesystem::exists(info.path, ec))
                    continue;

                auto ftime = std::filesystem::last_write_time(info.path, ec);
                uint64_t currentTime = static_cast<uint64_t>(ftime.time_since_epoch().count());

                if (currentTime != info.lastWriteTime)
                {
                    info.lastWriteTime = currentTime;
                    CompileShader(info);
                }
            }
        }

        /**
         * @brief Attempt to compile a shader and notify callbacks.
         * @param info The shader file to compile.
         * @return True if compilation succeeded.
         */
        bool CompileShader(const ShaderFileInfo& info)
        {
            auto startTime = std::chrono::high_resolution_clock::now();

            ShaderReloadEvent event;
            event.shaderName = info.shaderName;
            event.shaderPath = info.path;
            event.success = false;

            // Validate that the file exists before attempting compilation
            std::error_code ec;
            if (!std::filesystem::exists(info.path, ec))
            {
                event.errorMessage = "Shader file not found: " + info.path;
                event.compileLog = event.errorMessage;
                {
                    std::scoped_lock lock(m_compiledShaderMutex);
                    event.reusedPreviousBinary = m_compiledShaders.contains(info.shaderName);
                }
                auto endTime = std::chrono::high_resolution_clock::now();
                event.compilationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
                Spark::SimpleConsole::GetInstance().LogError("Shader hot-reload failed: " + event.errorMessage);
                NotifyReload(event);
                return false;
            }

            // Read shader source from disk
            std::string sourceCode;
            {
                std::ifstream file(info.path, std::ios::binary);
                if (!file.is_open())
                {
                    event.errorMessage = "Failed to open shader file: " + info.path;
                    event.compileLog = event.errorMessage;
                    {
                        std::scoped_lock lock(m_compiledShaderMutex);
                        event.reusedPreviousBinary = m_compiledShaders.contains(info.shaderName);
                    }
                    auto endTime = std::chrono::high_resolution_clock::now();
                    event.compilationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
                    Spark::SimpleConsole::GetInstance().LogError("Shader hot-reload failed: " + event.errorMessage);
                    NotifyReload(event);
                    return false;
                }
                sourceCode.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }

            // Validate shader source is non-empty
            if (sourceCode.empty())
            {
                event.errorMessage = "Shader file is empty: " + info.path;
                event.compileLog = event.errorMessage;
                {
                    std::scoped_lock lock(m_compiledShaderMutex);
                    event.reusedPreviousBinary = m_compiledShaders.contains(info.shaderName);
                }
                auto endTime = std::chrono::high_resolution_clock::now();
                event.compilationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
                Spark::SimpleConsole::GetInstance().LogError("Shader hot-reload failed: " + event.errorMessage);
                NotifyReload(event);
                return false;
            }

            Spark::RHI::ShaderCompileOptions options;
            options.stage = ToRHIStage(info.shaderType);
            options.sourceFile = info.path;
            options.sourceCode = sourceCode;
            options.entryPoint = "main";
            options.sourceLanguage = DetermineSourceLanguage(std::filesystem::path(info.path));
            // Leave targetLanguage Auto: RHI::CompileShader derives it from the
            // backend (D3D -> DXBC via d3dcompiler) and falls back to the source
            // language only when the backend is unknown.
            options.targetBackend = m_targetBackend;

            const Spark::RHI::ShaderCompileResult compileResult = Spark::RHI::CompileShader(options);
            event.success = compileResult.success;
            event.errorMessage = compileResult.errorMessage;
            event.compileLog = compileResult.errorMessage;

            if (event.success)
            {
                std::scoped_lock lock(m_compiledShaderMutex);
                m_compiledShaders[info.shaderName] = compileResult.bytecode;
                ++m_shaderSwapGenerations[info.shaderName];
            }
            else
            {
                std::scoped_lock lock(m_compiledShaderMutex);
                event.reusedPreviousBinary = m_compiledShaders.contains(info.shaderName);
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            event.compilationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

            if (event.success)
            {
                ++m_reloadCount;
                Spark::SimpleConsole::GetInstance().LogSuccess("Shader hot-reloaded: " + info.shaderName + " (" +
                                                               info.path + ")");
            }
            else
            {
                const std::string suffix = event.reusedPreviousBinary ? " (previous shader kept active)" : "";
                Spark::SimpleConsole::GetInstance().LogError("Shader hot-reload failed: " + info.shaderName + " — " +
                                                             event.errorMessage + suffix);
            }

            NotifyReload(event);
            return event.success;
        }

        /** @brief Invoke all registered reload callbacks. */
        void NotifyReload(const ShaderReloadEvent& event)
        {
            for (const auto& callback : m_reloadCallbacks)
            {
                callback(event);
            }
        }

        // -- State --
        std::map<std::string, ShaderFileInfo> m_watchedFiles; ///< Shader name -> file info.
        std::vector<std::string> m_watchDirectories;          ///< Watched root directories.
        std::set<std::string> m_watchDirectorySet;            ///< Canonical keys of the above.
        Spark::RHI::GraphicsBackend m_targetBackend = Spark::RHI::GraphicsBackend::Auto; ///< Reload compile target.
        float m_pollInterval = 0.5f;                                                     ///< Seconds between polls.
        float m_timeSinceLastPoll = 0.0f;                                                ///< Accumulator for polling.
        std::vector<std::function<void(const ShaderReloadEvent&)>> m_reloadCallbacks;    ///< Reload event subscribers.
        bool m_enabled = false;                                                          ///< Whether polling is active.
        uint32_t m_reloadCount = 0;                                                      ///< Total successful reloads.
        mutable std::mutex m_compiledShaderMutex;                      ///< Protects compiled shader maps.
        std::map<std::string, std::vector<uint8_t>> m_compiledShaders; ///< Active compiled binaries.
        std::map<std::string, uint64_t> m_shaderSwapGenerations;       ///< Atomic swap generation counter.
    };

} // namespace Spark::Graphics
