/**
 * @file ShaderDiskCache.cpp
 * @brief Persistent shader disk cache implementation
 */

#include "ShaderDiskCache.h"

#include "../Utils/LogMacros.h"

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

        std::lock_guard lock(m_mutex);
        uint64_t hash = HashSourceForDisk(source, target);
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

        std::lock_guard lock(m_mutex);
        uint64_t hash = HashSourceForDisk(source, target);
        auto path = GetBlobPath(hash, target, source.stage);

        std::ofstream ofs(path, std::ios::binary);
        if (ofs)
        {
            ofs.write(reinterpret_cast<const char*>(blob.bytecode.data()),
                      static_cast<std::streamsize>(blob.bytecode.size()));
        }
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
        // FNV-1a hash of source + defines + target + stage
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
