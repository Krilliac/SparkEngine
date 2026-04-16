/**
 * @file ShaderDiskCache.cpp
 * @brief Persistent shader disk cache implementation
 */

#include "ShaderDiskCache.h"

#include "../Utils/LogMacros.h"
#include "../Utils/ShaderServiceClient.h"
#include "ShaderDaemonBridge.h"

#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Spark::Graphics
{

    void ShaderDiskCache::Initialize(const std::filesystem::path& cacheDir)
    {
        std::lock_guard lock(m_mutex);
        m_cacheDir = cacheDir;

        std::error_code ec;
        std::filesystem::create_directories(m_cacheDir, ec);
        if (ec)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "ShaderDiskCache: failed to create '%s': %s",
                           m_cacheDir.string().c_str(), ec.message().c_str());
            return;
        }

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "ShaderDiskCache: initialized at '%s' (%zu entries)",
                       m_cacheDir.string().c_str(), GetEntryCount());
    }

    void ShaderDiskCache::Shutdown()
    {
        std::lock_guard lock(m_mutex);
        m_initialized = false;
    }

    std::optional<CompiledShaderBlob> ShaderDiskCache::Lookup(const ShaderSource& source, ShaderTarget target) const
    {
        if (!m_initialized)
            return std::nullopt;

        uint64_t hash = HashSourceForDisk(source, target);

        // Consult the daemon first if wired. On hit we return the full blob
        // (metadata + bytecode) — the daemon carries the complete
        // CompiledShaderBlob, unlike the local cache which is bytecode-only.
        //
        // The daemon client pointer is snapshotted under the lock and then
        // used unlocked: ShaderServiceClient serialises its own RPCs via the
        // underlying DaemonClient mutex, and the alternative (holding our
        // own mutex across a network round-trip) would serialise all local
        // disk operations behind the slowest daemon call.
        Spark::Daemon::ShaderServiceClient* daemon = nullptr;
        {
            std::lock_guard lock(m_mutex);
            daemon = m_daemonClient;
        }

        if (daemon)
        {
            auto response =
                daemon->GetCacheEntry(hash, static_cast<uint8_t>(target), static_cast<uint8_t>(source.stage));
            if (response && response->found)
            {
                CompiledShaderBlob decoded;
                if (DecodeCompiledShaderBlob(response->blob, decoded))
                {
                    m_daemonHits.fetch_add(1, std::memory_order_relaxed);
                    return decoded;
                }
            }
            // Miss, decode failure, or transport error all count as a miss —
            // fall through to local disk.
            m_daemonMisses.fetch_add(1, std::memory_order_relaxed);
        }

        std::lock_guard lock(m_mutex);
        auto path = GetBlobPath(hash, target, source.stage);

        if (!std::filesystem::exists(path))
            return std::nullopt;

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
            return std::nullopt;

        auto fileSize = std::filesystem::file_size(path);
        CompiledShaderBlob blob;
        blob.bytecode.resize(fileSize);
        ifs.read(reinterpret_cast<char*>(blob.bytecode.data()), static_cast<std::streamsize>(fileSize));

        if (!ifs)
            return std::nullopt;

        blob.target = target;
        blob.stage = source.stage;
        blob.entryPoint = source.entryPoint;
        blob.success = true;

        return blob;
    }

    void ShaderDiskCache::Store(const ShaderSource& source, ShaderTarget target, const CompiledShaderBlob& blob)
    {
        if (!m_initialized || !blob.success || blob.bytecode.empty())
            return;

        uint64_t hash = HashSourceForDisk(source, target);

        Spark::Daemon::ShaderServiceClient* daemon = nullptr;
        {
            std::lock_guard lock(m_mutex);
            daemon = m_daemonClient;
            auto path = GetBlobPath(hash, target, source.stage);

            std::ofstream ofs(path, std::ios::binary);
            if (ofs)
            {
                ofs.write(reinterpret_cast<const char*>(blob.bytecode.data()),
                          static_cast<std::streamsize>(blob.bytecode.size()));
            }
        }

        if (daemon)
        {
            // Best-effort push to the daemon. Transport errors are swallowed —
            // the local disk cache already holds the authoritative copy.
            (void)daemon->PutCacheEntry(hash, static_cast<uint8_t>(target), static_cast<uint8_t>(source.stage),
                                        EncodeCompiledShaderBlob(blob));
        }
    }

    void ShaderDiskCache::SetDaemonClient(Spark::Daemon::ShaderServiceClient* client)
    {
        std::lock_guard lock(m_mutex);
        m_daemonClient = client;
    }

    void ShaderDiskCache::Clear()
    {
        std::lock_guard lock(m_mutex);
        if (!m_initialized)
            return;

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(m_cacheDir, ec))
        {
            if (entry.path().extension() == ".blob")
                std::filesystem::remove(entry.path(), ec);
        }
    }

    size_t ShaderDiskCache::GetEntryCount() const
    {
        if (!m_initialized)
            return 0;

        size_t count = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(m_cacheDir, ec))
        {
            if (entry.path().extension() == ".blob")
                ++count;
        }
        return count;
    }

    size_t ShaderDiskCache::GetDiskUsage() const
    {
        if (!m_initialized)
            return 0;

        size_t totalBytes = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(m_cacheDir, ec))
        {
            if (entry.path().extension() == ".blob")
                totalBytes += entry.file_size(ec);
        }
        return totalBytes;
    }

    std::filesystem::path ShaderDiskCache::GetBlobPath(uint64_t hash, ShaderTarget target, ShaderStage stage) const
    {
        std::ostringstream filename;
        filename << std::hex << std::setfill('0') << std::setw(16) << hash << "_" << TargetName(target) << "_"
                 << StageName(stage) << ".blob";
        return m_cacheDir / filename.str();
    }

    uint64_t ShaderDiskCache::HashSourceForDisk(const ShaderSource& source, ShaderTarget target)
    {
        // FNV-1a hash of source + defines + target + stage + entry point.
        //
        // Fix for Codex P2 review comment on PR #459: the previous hash
        // omitted `entryPoint`, so two shaders compiled from the same HLSL
        // source / target / stage / defines but with different entry-point
        // functions (common when one .hlsl file exposes VSMain, VSShadowMain,
        // etc.) would collide and reuse each other's cached bytecode. Mixing
        // `entryPoint` into the FNV chain keeps the key-space separated.
        uint64_t hash = 14695981039346656037ULL;
        constexpr uint64_t prime = 1099511628211ULL;

        for (char c : source.hlslCode)
        {
            hash ^= static_cast<uint8_t>(c);
            hash *= prime;
        }
        for (const auto& d : source.defines)
        {
            for (char c : d)
            {
                hash ^= static_cast<uint8_t>(c);
                hash *= prime;
            }
        }
        hash ^= static_cast<uint8_t>(target);
        hash *= prime;
        hash ^= static_cast<uint8_t>(source.stage);
        hash *= prime;
        for (char c : source.entryPoint)
        {
            hash ^= static_cast<uint8_t>(c);
            hash *= prime;
        }

        return hash;
    }

    std::string ShaderDiskCache::TargetName(ShaderTarget target)
    {
        switch (target)
        {
        case ShaderTarget::DXIL:
            return "dxil";
        case ShaderTarget::DXBC:
            return "dxbc";
        case ShaderTarget::SPIRV:
            return "spirv";
        case ShaderTarget::GLSL:
            return "glsl";
        case ShaderTarget::GLSL_ES:
            return "glsl_es";
        case ShaderTarget::MSL:
            return "msl";
        case ShaderTarget::WGSL:
            return "wgsl";
        default:
            return "unknown";
        }
    }

    std::string ShaderDiskCache::StageName(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex:
            return "vs";
        case ShaderStage::Pixel:
            return "ps";
        case ShaderStage::Geometry:
            return "gs";
        case ShaderStage::Hull:
            return "hs";
        case ShaderStage::Domain:
            return "ds";
        case ShaderStage::Compute:
            return "cs";
        case ShaderStage::Mesh:
            return "ms";
        case ShaderStage::Amplification:
            return "as";
        default:
            return "unk";
        }
    }

} // namespace Spark::Graphics
