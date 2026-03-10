/**
 * @file ShaderCacheWarming.h
 * @brief Shader permutation precompilation and cache management
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

#include <vector>
#include <unordered_map>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstdint>
#include <cstring>
#include <fstream>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    enum class ShaderStage
    {
        Vertex,
        Pixel,
        Compute,
        Geometry,
        Hull,
        Domain
    };

    struct ShaderPermutation
    {
        std::string sourcePath;          ///< Shader source file path or identifier
        std::string entryPoint = "main"; ///< Entry point function name
        ShaderStage stage = ShaderStage::Pixel;
        std::vector<std::pair<std::string, std::string>> defines; ///< Preprocessor defines
        uint64_t hash = 0;                                        ///< Computed hash for cache lookup
    };

    struct ShaderCacheEntry
    {
        ComPtr<ID3DBlob> bytecode;
        ShaderStage stage;
        uint64_t sourceHash;
        bool compiled = false;

        // Compiled shader objects (only one will be valid based on stage)
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11ComputeShader> cs;
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11HullShader> hs;
        ComPtr<ID3D11DomainShader> ds;
    };

    struct ShaderCacheMetrics
    {
        uint32_t totalPermutations = 0;
        uint32_t compiledPermutations = 0;
        uint32_t failedPermutations = 0;
        uint32_t cacheHits = 0;
        uint32_t cacheMisses = 0;
        float warmingProgress = 0.0f; ///< 0.0 to 1.0
        bool isWarming = false;
    };

    /**
     * @class ShaderCacheWarming
     * @brief Precompiles shader permutations at startup to avoid runtime hitches
     *
     * Registers shader source + define combinations, then compiles them on a
     * background thread during loading screens or idle frames. Compiled shaders
     * are cached by hash for instant retrieval during rendering.
     *
     * Optionally serializes compiled bytecode to disk for faster subsequent loads.
     */
    class ShaderCacheWarming
    {
      public:
        using ProgressCallback = std::function<void(float progress, const std::string& currentShader)>;

        ShaderCacheWarming() = default;
        ~ShaderCacheWarming() { StopWarming(); }

        bool Initialize(ID3D11Device* device)
        {
            m_device = device;
            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            StopWarming();
            m_cache.clear();
            m_pendingPermutations.clear();
            m_initialized = false;
        }

        /**
         * @brief Register a shader permutation for precompilation
         */
        void RegisterPermutation(const std::string& source, const std::string& entryPoint, ShaderStage stage,
                                 const std::vector<std::pair<std::string, std::string>>& defines = {})
        {
            ShaderPermutation perm;
            perm.sourcePath = source;
            perm.entryPoint = entryPoint;
            perm.stage = stage;
            perm.defines = defines;
            perm.hash = ComputeHash(perm);

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_cache.find(perm.hash) == m_cache.end())
            {
                m_pendingPermutations.push_back(perm);
                m_metrics.totalPermutations++;
            }
        }

        /**
         * @brief Register common PBR shader variants
         */
        void RegisterPBRPermutations(const std::string& pbrShaderSource)
        {
            // Common PBR material feature combinations
            std::vector<std::vector<std::pair<std::string, std::string>>> variants = {
                {}, // Base PBR
                {{"HAS_NORMAL_MAP", "1"}},
                {{"HAS_NORMAL_MAP", "1"}, {"HAS_EMISSIVE", "1"}},
                {{"HAS_NORMAL_MAP", "1"}, {"HAS_METALLIC_ROUGHNESS", "1"}},
                {{"HAS_NORMAL_MAP", "1"}, {"HAS_METALLIC_ROUGHNESS", "1"}, {"HAS_EMISSIVE", "1"}},
                {{"HAS_NORMAL_MAP", "1"}, {"HAS_METALLIC_ROUGHNESS", "1"}, {"HAS_AO", "1"}},
                {{"HAS_NORMAL_MAP", "1"}, {"HAS_METALLIC_ROUGHNESS", "1"}, {"HAS_AO", "1"}, {"HAS_EMISSIVE", "1"}},
                {{"ALPHA_TEST", "1"}},
                {{"ALPHA_TEST", "1"}, {"HAS_NORMAL_MAP", "1"}},
                {{"HAS_SUBSURFACE", "1"}},
                {{"HAS_CLEARCOAT", "1"}},
                {{"HAS_ANISOTROPY", "1"}},
                {{"SKINNED", "1"}},
                {{"SKINNED", "1"}, {"HAS_NORMAL_MAP", "1"}},
                {{"INSTANCED", "1"}},
                {{"INSTANCED", "1"}, {"HAS_NORMAL_MAP", "1"}},
            };

            for (const auto& defines : variants)
            {
                RegisterPermutation(pbrShaderSource, "VSMain", ShaderStage::Vertex, defines);
                RegisterPermutation(pbrShaderSource, "PSMain", ShaderStage::Pixel, defines);
            }
        }

        /**
         * @brief Start background shader compilation
         */
        void StartWarming(ProgressCallback callback = nullptr)
        {
            if (!m_initialized || m_warming.load())
                return;

            m_warming.store(true);
            m_metrics.isWarming = true;
            m_progressCallback = callback;

            m_warmingThread = std::thread(
                [this]()
                {
                    CompileAllPending();
                    m_warming.store(false);
                    m_metrics.isWarming = false;
                    m_metrics.warmingProgress = 1.0f;
                });
        }

        /**
         * @brief Compile one pending shader per call (non-blocking, for idle-frame warming)
         * @return true if there are more shaders to compile
         */
        bool WarmOne()
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (m_pendingPermutations.empty())
                return false;

            ShaderPermutation perm = m_pendingPermutations.back();
            m_pendingPermutations.pop_back();

            CompilePermutation(perm);

            m_metrics.warmingProgress =
                static_cast<float>(m_metrics.compiledPermutations + m_metrics.failedPermutations) /
                static_cast<float>(m_metrics.totalPermutations);

            return !m_pendingPermutations.empty();
        }

        void StopWarming()
        {
            m_warming.store(false);
            if (m_warmingThread.joinable())
                m_warmingThread.join();
        }

        /**
         * @brief Look up a compiled shader by permutation hash
         */
        const ShaderCacheEntry* GetCachedShader(uint64_t hash) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cache.find(hash);
            if (it != m_cache.end() && it->second.compiled)
            {
                m_metrics.cacheHits++;
                return &it->second;
            }
            m_metrics.cacheMisses++;
            return nullptr;
        }

        /**
         * @brief Compute hash for a permutation (for external lookup)
         */
        static uint64_t ComputeHash(const ShaderPermutation& perm)
        {
            uint64_t hash = 14695981039346656037ULL; // FNV-1a offset basis
            auto hashStr = [&hash](const std::string& s)
            {
                for (char c : s)
                {
                    hash ^= static_cast<uint64_t>(c);
                    hash *= 1099511628211ULL; // FNV-1a prime
                }
            };

            hashStr(perm.sourcePath);
            hashStr(perm.entryPoint);
            hash ^= static_cast<uint64_t>(perm.stage);
            hash *= 1099511628211ULL;

            for (const auto& def : perm.defines)
            {
                hashStr(def.first);
                hashStr(def.second);
            }

            return hash;
        }

        /**
         * @brief Save compiled bytecode cache to disk
         */
        bool SaveCacheToDisk(const std::string& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::ofstream file(path, std::ios::binary);
            if (!file.is_open())
                return false;

            uint32_t entryCount = 0;
            for (const auto& [hash, entry] : m_cache)
            {
                if (entry.compiled && entry.bytecode)
                    entryCount++;
            }

            file.write(reinterpret_cast<const char*>(&entryCount), sizeof(entryCount));
            if (!file.good())
                return false;

            for (const auto& [hash, entry] : m_cache)
            {
                if (!entry.compiled || !entry.bytecode)
                    continue;

                file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
                uint32_t stageVal = static_cast<uint32_t>(entry.stage);
                file.write(reinterpret_cast<const char*>(&stageVal), sizeof(stageVal));
                uint32_t byteSize = static_cast<uint32_t>(entry.bytecode->GetBufferSize());
                file.write(reinterpret_cast<const char*>(&byteSize), sizeof(byteSize));
                file.write(reinterpret_cast<const char*>(entry.bytecode->GetBufferPointer()), byteSize);
                if (!file.good())
                    return false;
            }

            return true;
        }

        /**
         * @brief Load compiled bytecode cache from disk
         */
        bool LoadCacheFromDisk(const std::string& path)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
                return false;

            uint32_t entryCount;
            file.read(reinterpret_cast<char*>(&entryCount), sizeof(entryCount));

            for (uint32_t i = 0; i < entryCount; i++)
            {
                uint64_t hash;
                uint32_t stageVal, byteSize;

                file.read(reinterpret_cast<char*>(&hash), sizeof(hash));
                file.read(reinterpret_cast<char*>(&stageVal), sizeof(stageVal));
                file.read(reinterpret_cast<char*>(&byteSize), sizeof(byteSize));

                std::vector<uint8_t> bytecodeData(byteSize);
                file.read(reinterpret_cast<char*>(bytecodeData.data()), byteSize);

                ComPtr<ID3DBlob> bytecodeBlob;
                D3DCreateBlob(byteSize, &bytecodeBlob);
                memcpy(bytecodeBlob->GetBufferPointer(), bytecodeData.data(), byteSize);

                ShaderCacheEntry entry;
                entry.bytecode = bytecodeBlob;
                entry.stage = static_cast<ShaderStage>(stageVal);
                entry.sourceHash = hash;

                CreateShaderObject(entry);
                entry.compiled = true;

                m_cache[hash] = std::move(entry);
                m_metrics.compiledPermutations++;
                m_metrics.totalPermutations++;
            }

            return true;
        }

        const ShaderCacheMetrics& GetMetrics() const { return m_metrics; }

        std::string Console_GetStatus() const
        {
            std::string s = "Shader Cache:\n";
            s += "  Permutations: " + std::to_string(m_metrics.compiledPermutations) + "/" +
                 std::to_string(m_metrics.totalPermutations) + " compiled\n";
            s += "  Failed: " + std::to_string(m_metrics.failedPermutations) + "\n";
            s += "  Cache hits: " + std::to_string(m_metrics.cacheHits) +
                 ", misses: " + std::to_string(m_metrics.cacheMisses) + "\n";
            s += "  Warming: " + std::string(m_metrics.isWarming ? "In progress" : "Idle");
            if (m_metrics.isWarming)
                s += " (" + std::to_string(static_cast<int>(m_metrics.warmingProgress * 100)) + "%)";
            s += "\n";
            return s;
        }

      private:
        void CompileAllPending()
        {
            while (m_warming.load())
            {
                ShaderPermutation perm;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_pendingPermutations.empty())
                        break;
                    perm = m_pendingPermutations.back();
                    m_pendingPermutations.pop_back();
                }

                CompilePermutation(perm);

                float progress = static_cast<float>(m_metrics.compiledPermutations + m_metrics.failedPermutations) /
                                 static_cast<float>(m_metrics.totalPermutations);
                m_metrics.warmingProgress = progress;

                if (m_progressCallback)
                    m_progressCallback(progress, perm.sourcePath);
            }
        }

        void CompilePermutation(const ShaderPermutation& perm)
        {
            // Build D3D macro array from defines
            std::vector<D3D_SHADER_MACRO> macros;
            for (const auto& def : perm.defines)
            {
                macros.push_back({def.first.c_str(), def.second.c_str()});
            }
            macros.push_back({nullptr, nullptr}); // Null terminator

            const char* target = nullptr;
            switch (perm.stage)
            {
            case ShaderStage::Vertex:
                target = "vs_5_0";
                break;
            case ShaderStage::Pixel:
                target = "ps_5_0";
                break;
            case ShaderStage::Compute:
                target = "cs_5_0";
                break;
            case ShaderStage::Geometry:
                target = "gs_5_0";
                break;
            case ShaderStage::Hull:
                target = "hs_5_0";
                break;
            case ShaderStage::Domain:
                target = "ds_5_0";
                break;
            }

            ComPtr<ID3DBlob> blob, errorBlob;
            HRESULT hr =
                D3DCompile(perm.sourcePath.c_str(), perm.sourcePath.size(), nullptr, macros.data(), nullptr,
                           perm.entryPoint.c_str(), target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errorBlob);

            std::lock_guard<std::mutex> lock(m_mutex);

            if (SUCCEEDED(hr))
            {
                ShaderCacheEntry entry;
                entry.bytecode = blob;
                entry.stage = perm.stage;
                entry.sourceHash = perm.hash;
                CreateShaderObject(entry);
                entry.compiled = true;

                m_cache[perm.hash] = std::move(entry);
                m_metrics.compiledPermutations++;
            }
            else
            {
                m_metrics.failedPermutations++;
            }
        }

        void CreateShaderObject(ShaderCacheEntry& entry)
        {
            if (!entry.bytecode || !m_device)
                return;

            const void* code = entry.bytecode->GetBufferPointer();
            SIZE_T size = entry.bytecode->GetBufferSize();

            switch (entry.stage)
            {
            case ShaderStage::Vertex:
                m_device->CreateVertexShader(code, size, nullptr, &entry.vs);
                break;
            case ShaderStage::Pixel:
                m_device->CreatePixelShader(code, size, nullptr, &entry.ps);
                break;
            case ShaderStage::Compute:
                m_device->CreateComputeShader(code, size, nullptr, &entry.cs);
                break;
            case ShaderStage::Geometry:
                m_device->CreateGeometryShader(code, size, nullptr, &entry.gs);
                break;
            case ShaderStage::Hull:
                m_device->CreateHullShader(code, size, nullptr, &entry.hs);
                break;
            case ShaderStage::Domain:
                m_device->CreateDomainShader(code, size, nullptr, &entry.ds);
                break;
            }
        }

        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;

        mutable std::mutex m_mutex;
        std::unordered_map<uint64_t, ShaderCacheEntry> m_cache;
        std::vector<ShaderPermutation> m_pendingPermutations;

        std::atomic<bool> m_warming{false};
        std::thread m_warmingThread;
        ProgressCallback m_progressCallback;

        mutable ShaderCacheMetrics m_metrics;
    };

} // namespace Spark::Graphics
