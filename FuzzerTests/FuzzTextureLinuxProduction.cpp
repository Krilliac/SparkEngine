/**
 * @file FuzzTextureLinuxProduction.cpp
 * @brief libc++-compiled production adapter for the non-Windows texture libFuzzer harness.
 *
 * Texture::CreateFromFile takes a path and chooses its decoder from the file
 * extension, so the input is written to a private temporary file: `.exr` when
 * it starts with the OpenEXR magic (the bounded EXRLoader route), `.img`
 * otherwise (the stb_image BMP/TGA route). The texture is created from an
 * all-zero TextureDesc, so a load that reports success without decoding
 * anything is visible as a zero-sized texture. A violated invariant aborts so
 * libFuzzer records it as a crash rather than a silent pass.
 */

#include "FuzzTextureLinuxProduction.h"

#include "Graphics/EXRLoader.h"
#include "Graphics/TextureSystem.h"

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
    constexpr std::uint64_t kMaxDimension = 16384; // STBI_MAX_DIMENSIONS and EXRLoader's per-side cap

    [[noreturn]] void InfrastructureFailure(const char* operation)
    {
        std::fprintf(stderr, "SparkFuzzTextureLinux infrastructure failure during %s: %s\n", operation,
                     std::strerror(errno));
        std::_Exit(70);
    }

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzTextureLinux: Texture::CreateFromFile violated: %s\n", what);
        std::abort();
    }

    class TemporaryInput
    {
      public:
        explicit TemporaryInput(const char* suffix)
        {
            std::array<char, 64> pattern{};
            std::snprintf(pattern.data(), pattern.size(), "/tmp/spark-texture-fuzz-XXXXXX%s", suffix);
            m_descriptor = ::mkstemps(pattern.data(), static_cast<int>(std::strlen(suffix)));
            if (m_descriptor < 0)
                InfrastructureFailure("mkstemps");
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
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled texture loader, stb_image and tinyexr.
extern "C" int SparkFuzzCreateTextureFromFile(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const bool exrRoute = Spark::Graphics::EXRLoader::IsEXR(data, size);
    TemporaryInput input(exrRoute ? ".exr" : ".img");
    input.Write(data, size);

    Texture texture("fuzz", TextureDesc{});
    const HRESULT hr = texture.CreateFromFile(input.Path(), nullptr);

    if (FAILED(hr))
    {
        if (texture.IsLoaded() || texture.GetMemoryUsage() != 0)
            InvariantFailure("a failed load left the texture loaded or sized");
        return 0;
    }

    if (!texture.IsLoaded())
        InvariantFailure("a successful load did not mark the texture loaded");
    const TextureDesc& desc = texture.GetDesc();
    if (desc.width == 0 || desc.height == 0 || desc.width > kMaxDimension || desc.height > kMaxDimension)
        InvariantFailure("a successful load produced dimensions outside [1, 16384]");
    const bool isFloat = desc.format == TextureFormat::R32G32B32A32_FLOAT;
    if (exrRoute != isFloat)
        InvariantFailure("pixel format does not match the decoder route");
    const std::uint64_t bytesPerPixel = isFloat ? 16u : 4u;
    if (texture.GetMemoryUsage() != static_cast<std::uint64_t>(desc.width) * desc.height * bytesPerPixel)
        InvariantFailure("memory usage does not match the decoded dimensions");
    return 0;
}
