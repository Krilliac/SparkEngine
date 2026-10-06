/**
 * @file FuzzTextureStexProduction.cpp
 * @brief libc++-compiled production adapter for the .stex libFuzzer harness.
 *
 * LoadCompressed takes a path, so the fuzz input is written to a private
 * temporary file and the shipped loader reads it back. A texture the loader
 * accepts is then handed to Decompress (the in-engine consumer) and checked
 * against the invariants the loader promises; a violation aborts so libFuzzer
 * records it as a crash rather than a silent pass.
 */

#include "FuzzTextureStexProduction.h"

#include "Graphics/TextureCompressor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InfrastructureFailure(const char* operation)
    {
        std::fprintf(stderr, "SparkFuzzTextureStex infrastructure failure during %s: %s\n", operation,
                     std::strerror(errno));
        std::_Exit(70);
    }

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzTextureStex: LoadCompressed accepted a texture that violates: %s\n", what);
        std::abort();
    }

    class TemporaryInput
    {
      public:
        TemporaryInput()
        {
            std::array<char, 64> pattern{};
            std::snprintf(pattern.data(), pattern.size(), "/tmp/spark-stex-fuzz-XXXXXX");
            m_descriptor = ::mkstemp(pattern.data());
            if (m_descriptor < 0)
                InfrastructureFailure("mkstemp");
            m_path = pattern.data();
        }

        TemporaryInput(const TemporaryInput&) = delete;
        TemporaryInput& operator=(const TemporaryInput&) = delete;

        ~TemporaryInput()
        {
            if (m_descriptor >= 0)
                ::close(m_descriptor);
            if (!m_path.empty())
                ::unlink(m_path.c_str());
        }

        void Write(const std::uint8_t* data, std::size_t size)
        {
            std::size_t offset = 0;
            while (offset < size)
            {
                const ssize_t written = ::write(m_descriptor, data + offset, size - offset);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    InfrastructureFailure("write");
                offset += static_cast<std::size_t>(written);
            }
            if (::close(m_descriptor) != 0)
                InfrastructureFailure("close");
            m_descriptor = -1;
        }

        const std::string& Path() const { return m_path; }

      private:
        int m_descriptor = -1;
        std::string m_path;
    };

    void CheckAcceptedTexture(Spark::Graphics::TextureCompressor& compressor,
                              const Spark::Graphics::CompressedTexture& tex)
    {
        using Spark::Graphics::TextureCompressor;
        if (tex.width == 0 && tex.height == 0 && tex.mipLevels == 0 && tex.mipData.empty())
            return; // rejected

        if (tex.width == 0 || tex.height == 0 || tex.width > TextureCompressor::kMaxStexDimension ||
            tex.height > TextureCompressor::kMaxStexDimension)
            InvariantFailure("dimension bound");
        if (tex.mipLevels == 0 || tex.mipLevels > TextureCompressor::kMaxStexMipLevels ||
            tex.mipData.size() != tex.mipLevels)
            InvariantFailure("mip count bound");

        std::uint32_t mipW = tex.width;
        std::uint32_t mipH = tex.height;
        for (std::uint32_t m = 0; m < tex.mipLevels; ++m)
        {
            if (tex.mipData[m].size() != compressor.EstimateCompressedSize(mipW, mipH, tex.format))
                InvariantFailure("mip payload size");
            // Decompress sizes its output from the (now bounded) dimensions.
            (void)compressor.Decompress(tex, m);
            mipW = std::max(mipW / 2, 1u);
            mipH = std::max(mipH / 2, 1u);
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production loader and logger sources.
extern "C" int SparkFuzzLoadStex(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    TemporaryInput input;
    input.Write(data, size);

    auto& compressor = Spark::Graphics::TextureCompressor::GetInstance();
    const Spark::Graphics::CompressedTexture tex = compressor.LoadCompressed(input.Path());
    CheckAcceptedTexture(compressor, tex);
    return 0;
}
