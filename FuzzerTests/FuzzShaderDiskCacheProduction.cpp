/**
 * @file FuzzShaderDiskCacheProduction.cpp
 * @brief libc++-compiled production adapter for the shader disk-cache libFuzzer harness.
 *
 * Shader::Initialize points the process-wide ShaderDiskCache at a directory any process of
 * the user can write, and every compile first asks ShaderDiskCache::Lookup, which hands the
 * cached file's bytes to the driver as bytecode. The adapter initializes a real cache in a
 * private directory, lets Store create the entry for one shader, then overwrites that file
 * with the fuzz bytes (a planted or torn cache entry) before each Lookup. A violated contract
 * aborts so libFuzzer records a crash:
 *  - Store wrote exactly one .blob entry for the shader (the planted file is the one Lookup reads),
 *  - a hit returns exactly the bytes on disk, never an empty bytecode buffer (Store never
 *    writes an empty entry, and an empty "successful" blob would be handed to the driver),
 *  - a hit is marked successful and carries the requested target, stage and entry point,
 *  - Lookup gives the same answer twice.
 */

#include "FuzzShaderDiskCacheProduction.h"

#include "Graphics/ShaderDiskCache.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 1024u * 1024u;
    constexpr auto kTarget = Spark::Graphics::ShaderTarget::SPIRV;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzShaderDiskCache: ShaderDiskCache::Lookup violated: %s\n", what);
        std::abort();
    }

    Spark::Graphics::ShaderSource MakeSource()
    {
        Spark::Graphics::ShaderSource source;
        source.hlslCode = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
        source.entryPoint = "VSMain";
        source.stage = Spark::Graphics::ShaderStage::Vertex;
        source.defines = {"SPARK_FUZZ=1"};
        return source;
    }

    struct Fixture
    {
        Spark::Graphics::ShaderDiskCache cache;
        Spark::Graphics::ShaderSource source = MakeSource();
        std::filesystem::path blob;
    };

    void Setup(Fixture& fixture)
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "spark-fuzz-shadercache-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr)
            InvariantFailure("could not create the cache directory");
        fixture.cache.Initialize(pattern);
        if (!fixture.cache.IsInitialized())
            InvariantFailure("the cache did not initialize");

        Spark::Graphics::CompiledShaderBlob blob;
        blob.bytecode = {0x03, 0x02, 0x23, 0x07};
        blob.success = true;
        fixture.cache.Store(fixture.source, kTarget, blob);
        for (const auto& entry : std::filesystem::directory_iterator(pattern))
        {
            if (entry.path().extension() == ".blob")
            {
                if (!fixture.blob.empty())
                    InvariantFailure("Store wrote more than one entry for one shader");
                fixture.blob = entry.path();
            }
        }
        if (fixture.blob.empty())
            InvariantFailure("Store wrote no cache entry");
    }

    Fixture& GetFixture()
    {
        static Fixture fixture;
        static const bool ready = (Setup(fixture), true);
        (void)ready;
        return fixture;
    }

    void Plant(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            InvariantFailure("could not plant the cache entry");
    }

    void CheckHit(const Spark::Graphics::CompiledShaderBlob& blob, const Fixture& fixture, std::string_view onDisk)
    {
        if (blob.bytecode.empty())
            InvariantFailure("a hit returned empty bytecode");
        if (std::string_view(reinterpret_cast<const char*>(blob.bytecode.data()), blob.bytecode.size()) != onDisk)
            InvariantFailure("a hit returned bytes that differ from the cached file");
        if (!blob.success || blob.target != kTarget || blob.stage != fixture.source.stage ||
            blob.entryPoint != fixture.source.entryPoint)
            InvariantFailure("a hit does not describe the requested shader");
    }
} // namespace

extern "C" int SparkFuzzLookupShaderDiskCache(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    Fixture& fixture = GetFixture();
    const std::string_view bytes =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);
    Plant(fixture.blob, bytes);

    const std::optional<Spark::Graphics::CompiledShaderBlob> first = fixture.cache.Lookup(fixture.source, kTarget);
    const std::optional<Spark::Graphics::CompiledShaderBlob> second = fixture.cache.Lookup(fixture.source, kTarget);
    if (first.has_value() != second.has_value())
        InvariantFailure("Lookup gave different answers for the same file");
    if (!first)
        return 0;
    CheckHit(*first, fixture, bytes);
    CheckHit(*second, fixture, bytes);
    return 0;
}
