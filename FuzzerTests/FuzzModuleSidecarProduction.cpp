/**
 * @file FuzzModuleSidecarProduction.cpp
 * @brief libc++-compiled production adapter for the .sparkabi sidecar libFuzzer harness.
 *
 * ModuleManager::LoadModule and DiscoverModuleCandidates call
 * Spark::ModuleSidecar::ValidateModuleSidecar before the OS loader maps a game module, so
 * the sidecar is the last check standing between a dropped-in file and code execution.
 * The adapter keeps a fixed module image in a private directory and writes the fuzz bytes
 * as its `.sparkabi` sidecar, so the gate reads, parses and hashes real files.
 *
 * The compiler ABI version is the only descriptor field that differs between the Clang
 * releases that build this target, so the literal token `@compiler_abi_version@` in the
 * input is replaced by this build's value before the file is written; the committed
 * seeds stay valid on every toolchain. Expansion never lengthens the input.
 *
 * A violated contract aborts so libFuzzer records a crash:
 *  - the gate accepts the adapter's own correct sidecar (a gate that rejects everything
 *    would otherwise pass every other check),
 *  - a rejection always carries a reason, and the verdict is the same on a second call,
 *  - an accepted sidecar is at most 4096 bytes of exactly twelve distinct key=value lines
 *    whose descriptor fields equal this host's expected descriptor (struct_size may be
 *    larger: the descriptor is append-only) and whose
 *    binary_sha256 is the SHA-256 of the module image.
 */

#include "FuzzModuleSidecarProduction.h"

#include "Core/ModuleSidecar.h"
#include "Spark/ModuleABI.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
    constexpr std::size_t kMaxInputBytes = 8192;
    constexpr std::size_t kMaxSidecarBytes = 4096;
    constexpr std::string_view kAbiPlaceholder = "@compiler_abi_version@";

    // The module image the sidecar describes, and its SHA-256 (generate_module_sidecar_corpus.py
    // writes the same digest into the seeds).
    constexpr std::string_view kModuleImage = "SparkFuzzModuleSidecar image: not a loadable module.\n";
    constexpr std::string_view kModuleImageSha256 = "fddf32a6021977d4a4a3d266f98035c2200e203c7d44232b8a2312997ec0ffcc";

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzModuleSidecar: ValidateModuleSidecar violated: %s\n", what);
        std::abort();
    }

    void WriteFile(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            InvariantFailure("could not write a fixture file");
    }

    std::string Expand(std::string_view input)
    {
        const std::string abi = std::to_string(Spark::kExpectedModuleCompatibility.compilerABIVersion);
        std::string out;
        out.reserve(input.size());
        std::size_t position = 0;
        while (true)
        {
            const std::size_t found = input.find(kAbiPlaceholder, position);
            out.append(input.substr(position, found - position));
            if (found == std::string_view::npos)
                break;
            out.append(abi);
            position = found + kAbiPlaceholder.size();
        }
        return out;
    }

    std::map<std::string, std::uint32_t> ExpectedFields()
    {
        const SparkModuleCompatibilityDescriptor& d = Spark::kExpectedModuleCompatibility;
        return {{"struct_size", d.structSize},
                {"magic", d.magic},
                {"format", d.descriptorVersion},
                {"sdk_version", d.sdkVersion},
                {"runtime_abi_version", d.runtimeABIVersion},
                {"compiler_family", d.compilerFamily},
                {"compiler_abi_version", d.compilerABIVersion},
                {"cxx_language_level", d.cxxLanguageLevel},
                {"runtime_library", d.runtimeLibrary},
                {"iterator_debug_level", d.iteratorDebugLevel},
                {"pointer_size", d.pointerSize}};
    }

    std::string CorrectSidecar()
    {
        std::string text;
        for (const auto& [key, value] : ExpectedFields())
            text += key + "=" + std::to_string(value) + "\n";
        text += "binary_sha256=" + std::string(kModuleImageSha256) + "\n";
        return text;
    }

    /// Independent reading of what an accepted sidecar must say.
    void CheckAccepted(const std::string& sidecar)
    {
        if (sidecar.size() > kMaxSidecarBytes)
            InvariantFailure("an oversized sidecar was accepted");
        std::map<std::string, std::string> values;
        std::string_view remaining = sidecar;
        while (!remaining.empty())
        {
            const std::size_t end = remaining.find('\n');
            std::string_view line = remaining.substr(0, end);
            remaining = end == std::string_view::npos ? std::string_view() : remaining.substr(end + 1);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            const std::size_t separator = line.find('=');
            if (separator == std::string_view::npos || separator == 0 || separator + 1 >= line.size())
                InvariantFailure("an accepted sidecar has a malformed line");
            if (!values.emplace(std::string(line.substr(0, separator)), std::string(line.substr(separator + 1))).second)
                InvariantFailure("an accepted sidecar repeats a key");
        }
        if (values.size() != 12)
            InvariantFailure("an accepted sidecar does not hold exactly twelve fields");
        for (const auto& [key, expected] : ExpectedFields())
        {
            const auto it = values.find(key);
            if (it == values.end())
                InvariantFailure("an accepted sidecar lacks a descriptor field");
            std::uint32_t parsed = 0;
            const char* first = it->second.data();
            const char* last = first + it->second.size();
            const auto [end, error] = std::from_chars(first, last, parsed);
            if (error != std::errc{} || end != last)
                InvariantFailure("an accepted sidecar holds a field that is not a decimal uint32");
            // The descriptor is append-only (Spark/ModuleABI.h): a larger struct_size is a
            // later layout whose extra fields this host never reads. Every other field is
            // exact-match.
            const bool matches = key == "struct_size" ? parsed >= expected : parsed == expected;
            if (!matches)
                InvariantFailure("an accepted sidecar declares a descriptor this host does not expect");
        }
        const auto hash = values.find("binary_sha256");
        if (hash == values.end() || hash->second != kModuleImageSha256)
            InvariantFailure("an accepted sidecar does not carry the module image's SHA-256");
    }

    struct Fixture
    {
        std::filesystem::path module;
        std::filesystem::path sidecar;
    };

    const Fixture& GetFixture()
    {
        static const Fixture fixture = []
        {
            std::string pattern = (std::filesystem::temp_directory_path() / "spark-fuzz-sparkabi-XXXXXX").string();
            if (::mkdtemp(pattern.data()) == nullptr)
                InvariantFailure("could not create the fixture directory");
            const std::filesystem::path module = std::filesystem::path(pattern) / "libSparkFuzzModule.so";
            WriteFile(module, kModuleImage);
            const Fixture result{module, Spark::ModuleSidecar::SidecarPath(module)};
            WriteFile(result.sidecar, CorrectSidecar());
            std::string error;
            if (!Spark::ModuleSidecar::ValidateModuleSidecar(module, error))
            {
                std::fprintf(stderr, "SparkFuzzModuleSidecar: correct sidecar rejected: %s\n", error.c_str());
                InvariantFailure("the gate rejects a correct sidecar");
            }
            return result;
        }();
        return fixture;
    }
} // namespace

extern "C" int SparkFuzzValidateModuleSidecar(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    const Fixture& fixture = GetFixture();
    const std::string sidecar =
        Expand(size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size));
    WriteFile(fixture.sidecar, sidecar);

    std::string error;
    const bool accepted = Spark::ModuleSidecar::ValidateModuleSidecar(fixture.module, error);
    std::string secondError;
    if (Spark::ModuleSidecar::ValidateModuleSidecar(fixture.module, secondError) != accepted || secondError != error)
        InvariantFailure("the verdict changed on a second call");
    if (!accepted)
    {
        if (error.empty())
            InvariantFailure("a rejected sidecar carries no reason");
        return 0;
    }
    CheckAccepted(sidecar);
    return 0;
}
