/**
 * @file FuzzAssetCookerProduction.cpp
 * @brief libc++-compiled production adapter for the asset cooker libFuzzer harness.
 *
 * Spark::AssetPipeline::CookAssets (SparkAssetPipelineCore/src/AssetCooker.cpp) walks an asset
 * source tree that contributors and mods fill, copies every regular file into a fresh cook
 * generation, writes a JSON manifest of paths, SHA-256 digests and sizes, and publishes the
 * generation as the output root. The adapter turns the fuzz bytes into a source tree:
 *
 *   entry := [kind u8][name length u8][name][content length u8][content]   (at most 12)
 *   kind % 6: 0 regular file, 1 directory, 2 link to a canary file outside the tree,
 *             3 link to a canary directory outside the tree, 4 FIFO,
 *             5 swap: once the first asset is cooked, replace this regular file with a link to
 *               the canary file (the walk has already checked it by then)
 *   name: '/'-separated components; NUL becomes '_'; empty, "." and ".." components are
 *         dropped; at most SPARK_FUZZ_MAX_DEPTH components of at most 40 bytes
 *
 * It cooks the tree as a dry run, for real, and again. A violated contract aborts so libFuzzer
 * records a crash:
 *  - a cook fails only for a documented reason that applies to the tree (an asset path that is
 *    not UTF-8, a source entry changed while cooking, an asset named like the manifest), and a
 *    failed or dry-run cook publishes nothing and leaves no staging directory,
 *  - a successful cook's records are exactly the tree's regular files (an independent walk that
 *    follows no link), sorted, with UTF-8 paths free of empty, "." and ".." components, the
 *    sizes and independently computed SHA-256 digests of their bytes,
 *  - the output root holds exactly those files with those bytes plus the manifest, which is
 *    strict JSON listing the same records under schemaVersion 1 and the result's manifestSha256,
 *  - the canary's bytes never appear in the output,
 *  - cooking the unchanged tree again changes nothing and gives the same manifest digest.
 */

#include "FuzzAssetCookerProduction.h"

#include "SparkAssetPipelineCore/AssetCooker.h"
#include "Utils/JsonUtils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 4096;
    constexpr std::size_t kMaxEntries = 12;
    constexpr std::size_t kMaxComponents = 6;
    constexpr std::size_t kMaxComponentBytes = 40;
    constexpr std::string_view kCanary = "SPARK-COOK-CANARY-OUTSIDE-THE-SOURCE-TREE";
    constexpr std::string_view kManifestName = "spark-cook-manifest.json";
    namespace fs = std::filesystem;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzAssetCooker: CookAssets violated: %s\n", what);
        std::abort();
    }

    // ------------------------------------------------------------------
    // Independent SHA-256 (FIPS 180-4), so the digest oracle does not trust the cooker's own.
    // ------------------------------------------------------------------
    std::string Sha256Hex(std::string_view bytes)
    {
        static constexpr std::array<std::uint32_t, 64> k = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        const auto rotr = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); };
        std::string message(bytes);
        const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8u;
        message.push_back(static_cast<char>(0x80));
        while (message.size() % 64 != 56)
        {
            message.push_back('\0');
        }
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            message.push_back(static_cast<char>((bitLength >> shift) & 0xffu));
        }
        for (std::size_t block = 0; block < message.size(); block += 64)
        {
            std::array<std::uint32_t, 64> w{};
            for (std::size_t i = 0; i < 16; ++i)
            {
                const auto byte = [&](std::size_t j)
                { return static_cast<std::uint32_t>(static_cast<unsigned char>(message[block + 4 * i + j])); };
                w[i] = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
            }
            for (std::size_t i = 16; i < 64; ++i)
            {
                const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            std::array<std::uint32_t, 8> v = h;
            for (std::size_t i = 0; i < 64; ++i)
            {
                const std::uint32_t t1 = v[7] + (rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25)) +
                                         ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
                const std::uint32_t t2 =
                    (rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
                v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
            }
            for (std::size_t i = 0; i < 8; ++i)
            {
                h[i] += v[i];
            }
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string hex;
        for (const std::uint32_t word : h)
        {
            for (int shift = 28; shift >= 0; shift -= 4)
            {
                hex.push_back(kHex[(word >> shift) & 0xfu]);
            }
        }
        return hex;
    }

    /// Strict UTF-8 (RFC 3629), written independently of the cooker's check.
    bool IsUtf8(std::string_view text)
    {
        for (std::size_t i = 0; i < text.size();)
        {
            const auto c = static_cast<unsigned char>(text[i]);
            const std::size_t length = c < 0x80           ? 1
                                       : (c >> 5) == 0x6  ? 2
                                       : (c >> 4) == 0xe  ? 3
                                       : (c >> 3) == 0x1e ? 4
                                                          : 0;
            if (length == 0 || text.size() - i < length)
            {
                return false;
            }
            std::uint32_t cp = length == 1 ? c : length == 2 ? (c & 0x1fu) : length == 3 ? (c & 0x0fu) : (c & 0x07u);
            for (std::size_t j = 1; j < length; ++j)
            {
                const auto cc = static_cast<unsigned char>(text[i + j]);
                if ((cc & 0xc0u) != 0x80u)
                {
                    return false;
                }
                cp = (cp << 6) | (cc & 0x3fu);
            }
            static constexpr std::array<std::uint32_t, 5> kMinimum = {0, 0, 0x80, 0x800, 0x10000};
            if (cp < kMinimum[length] || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
            {
                return false;
            }
            i += length;
        }
        return true;
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    struct Fixture
    {
        fs::path base;
        fs::path source;
        fs::path output;
        fs::path canaryFile;
        fs::path canaryDirectory;
    };

    Fixture MakeFixture()
    {
        std::string pattern = (fs::temp_directory_path() / "spark-fuzz-cooker-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr)
        {
            InvariantFailure("could not create the fixture directory");
        }
        Fixture fixture;
        fixture.base = fs::canonical(pattern);
        fixture.source = fixture.base / "src";
        fixture.output = fixture.base / "out";
        fixture.canaryFile = fixture.base / "outside" / "canary.txt";
        fixture.canaryDirectory = fixture.base / "outside" / "dir";
        fs::create_directories(fixture.canaryDirectory);
        std::ofstream(fixture.canaryFile, std::ios::binary) << kCanary;
        std::ofstream(fixture.canaryDirectory / "inner.txt", std::ios::binary) << kCanary;
        return fixture;
    }

    struct Entry
    {
        std::uint8_t kind = 0;
        std::vector<std::string> components;
        std::string content;
    };

    std::vector<Entry> ParseEntries(std::string_view input)
    {
        std::vector<Entry> entries;
        std::size_t pos = 0;
        while (pos + 2 <= input.size() && entries.size() < kMaxEntries)
        {
            Entry entry;
            entry.kind = static_cast<std::uint8_t>(static_cast<unsigned char>(input[pos]) % 6u);
            const std::size_t nameLength = static_cast<unsigned char>(input[pos + 1]);
            pos += 2;
            const std::string_view name = input.substr(pos, std::min(nameLength, input.size() - pos));
            pos += name.size();
            const std::size_t contentLength = pos < input.size() ? static_cast<unsigned char>(input[pos]) : 0;
            pos += pos < input.size() ? 1 : 0;
            entry.content = std::string(input.substr(pos, std::min(contentLength, input.size() - pos)));
            pos += entry.content.size();

            std::string component;
            const auto flush = [&]
            {
                if (!component.empty() && component != "." && component != ".." &&
                    entry.components.size() < kMaxComponents)
                    entry.components.push_back(component.substr(0, kMaxComponentBytes));
                component.clear();
            };
            for (const char c : name)
            {
                if (c == '/')
                {
                    flush();
                }
                else
                    component.push_back(c == '\0' ? '_' : c);
            }
            flush();
            if (!entry.components.empty())
            {
                entries.push_back(std::move(entry));
            }
        }
        return entries;
    }

    /// Create the parents of @p entry under @p root as real directories; false when one of them
    /// already exists as anything else (a file, a link, a FIFO), so nothing is written through a
    /// link out of the tree.
    bool MakeParents(const fs::path& root, const Entry& entry, fs::path& path)
    {
        path = root;
        for (std::size_t i = 0; i + 1 < entry.components.size(); ++i)
        {
            path /= entry.components[i];
            std::error_code ec;
            const fs::file_status status = fs::symlink_status(path, ec);
            if (status.type() == fs::file_type::not_found)
            {
                if (!fs::create_directory(path, ec) || ec)
                {
                    return false;
                }
            }
            else if (status.type() != fs::file_type::directory)
            {
                return false;
            }
        }
        path /= entry.components.back();
        return true;
    }

    std::vector<std::string> BuildTree(const Fixture& fixture, const std::vector<Entry>& entries)
    {
        std::vector<std::string> swapTargets;
        for (const Entry& entry : entries)
        {
            fs::path path;
            if (!MakeParents(fixture.source, entry, path))
            {
                continue;
            }
            std::error_code ec;
            const fs::file_type existing = fs::symlink_status(path, ec).type();
            switch (entry.kind)
            {
            case 0:
                if (existing == fs::file_type::not_found || existing == fs::file_type::regular)
                {
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    out.write(entry.content.data(), static_cast<std::streamsize>(entry.content.size()));
                }
                break;
            case 1:
                if (existing == fs::file_type::not_found)
                {
                    fs::create_directory(path, ec);
                }
                break;
            case 2:
                if (existing == fs::file_type::not_found)
                {
                    fs::create_symlink(fixture.canaryFile, path, ec);
                }
                break;
            case 3:
                if (existing == fs::file_type::not_found)
                {
                    fs::create_directory_symlink(fixture.canaryDirectory, path, ec);
                }
                break;
            case 4:
                if (existing == fs::file_type::not_found)
                {
                    ::mkfifo(path.c_str(), 0600);
                }
                break;
            default:
            {
                std::string relative;
                for (const std::string& component : entry.components)
                {
                    relative.append(relative.empty() ? "" : "/").append(component);
                }
                swapTargets.push_back(relative);
                break;
            }
            }
        }
        return swapTargets;
    }

    /// The tree's regular files, by an independent walk that follows no link.
    std::map<std::string, std::string> RegularFiles(const fs::path& root)
    {
        std::map<std::string, std::string> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code statusError;
            if (it->symlink_status(statusError).type() == fs::file_type::regular && !statusError)
            {
                files[it->path().lexically_relative(root).generic_string()] = ReadFile(it->path());
            }
        }
        if (ec)
        {
            InvariantFailure("the fixture source tree could not be walked");
        }
        return files;
    }

    bool HasStagingSibling(const Fixture& fixture)
    {
        std::error_code ec;
        for (fs::directory_iterator it(fixture.base, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->path().filename().string().starts_with("out.spark-stage-"))
            {
                return true;
            }
        }
        return false;
    }

    void CheckNoCanary(const Fixture& fixture)
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(fixture.output, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code statusError;
            if (it->symlink_status(statusError).type() == fs::file_type::regular &&
                ReadFile(it->path()).find(kCanary) != std::string::npos)
                InvariantFailure("bytes from outside the source tree were cooked");
        }
    }

    void CheckRecords(const Spark::AssetPipeline::CookResult& result, const std::map<std::string, std::string>& files)
    {
        if (result.records.size() != files.size())
        {
            InvariantFailure("the records are not exactly the tree's regular files");
        }
        auto file = files.begin();
        for (std::size_t i = 0; i < result.records.size(); ++i, ++file)
        {
            const Spark::AssetPipeline::CookRecord& record = result.records[i];
            if (record.path != file->first)
            {
                InvariantFailure("a record names a path that is not the next regular file");
            }
            if (!IsUtf8(record.path) || record.path.starts_with('/'))
            {
                InvariantFailure("a record path is not a relative UTF-8 path");
            }
            for (const std::string_view bad : {"//", "/./", "/../"})
            {
                if (("/" + record.path + "/").find(bad) != std::string::npos)
                {
                    InvariantFailure("a record path holds an empty, '.' or '..' component");
                }
            }
            if (record.size != file->second.size() || record.sha256 != Sha256Hex(file->second))
            {
                InvariantFailure("a record's size or SHA-256 does not describe the file's bytes");
            }
        }
    }

    void CheckPublishedOutput(const Fixture& fixture, const Spark::AssetPipeline::CookResult& result,
                              const std::map<std::string, std::string>& files)
    {
        std::map<std::string, std::string> published = RegularFiles(fixture.output);
        const auto manifest = published.find(std::string(kManifestName));
        if (manifest == published.end())
        {
            InvariantFailure("the published output has no manifest");
        }
        const std::string manifestText = manifest->second;
        published.erase(manifest);
        if (published != files)
        {
            InvariantFailure("the published output does not hold exactly the cooked files and bytes");
        }

        Spark::Json::Value root;
        if (!Spark::Json::ParseStrict(manifestText, &root) || !root.IsObject())
        {
            InvariantFailure("the manifest is not strict JSON");
        }
        if (!root["schemaVersion"].IsNumber() || root["schemaVersion"].AsNumber() != 1.0 ||
            !root["manifestSha256"].IsString() || root["manifestSha256"].AsString() != result.manifestSha256)
            InvariantFailure("the manifest header does not match the result");
        const Spark::Json::Value& assets = root["assets"];
        if (!assets.IsArray() || assets.Size() != result.records.size())
        {
            InvariantFailure("the manifest does not list every record");
        }
        for (std::size_t i = 0; i < assets.Size(); ++i)
        {
            const Spark::Json::Value& asset = assets[i];
            const Spark::AssetPipeline::CookRecord& record = result.records[i];
            if (!asset["path"].IsString() || asset["path"].AsString() != record.path || !asset["sha256"].IsString() ||
                asset["sha256"].AsString() != record.sha256 || !asset["size"].IsNumber() ||
                asset["size"].AsNumber() != static_cast<double>(record.size))
                InvariantFailure("a manifest entry differs from its record");
        }
    }
} // namespace

extern "C" int SparkFuzzCookAssetTree(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    static const Fixture fixture = MakeFixture();
    std::error_code ec;
    fs::remove_all(fixture.source, ec);
    fs::remove_all(fixture.output, ec);
    fs::create_directories(fixture.source);

    const std::string_view input =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);
    const std::vector<std::string> swapTargets = BuildTree(fixture, ParseEntries(input));
    const std::map<std::string, std::string> files = RegularFiles(fixture.source);
    const bool nonUtf8 = std::any_of(files.begin(), files.end(), [](const auto& file) { return !IsUtf8(file.first); });
    const bool manifestClash = files.contains(std::string(kManifestName));

    // Dry run: nothing is published, and only a non-UTF-8 path can refuse it.
    const auto dry = Spark::AssetPipeline::CookAssets({fixture.source, fixture.output, {}, true});
    if (fs::exists(fixture.output, ec) || HasStagingSibling(fixture))
    {
        InvariantFailure("a dry run published output or left a staging directory");
    }
    if (dry.Succeeded() == nonUtf8 || (!dry.Succeeded() && dry.error.find("portable UTF-8") == std::string::npos))
    {
        InvariantFailure("the dry run failed for no documented reason, or accepted a non-UTF-8 path");
    }
    if (dry.Succeeded())
    {
        CheckRecords(dry, files);
        if (dry.updatedCount != files.size())
        {
            InvariantFailure("a dry run into an empty output did not mark every asset updated");
        }
    }

    // The real cook, with every swap target replaced by a link once the first asset is cooked.
    bool swapped = false;
    Spark::AssetPipeline::CookRequest request{fixture.source, fixture.output, {}, false};
    request.onProgress = [&](const Spark::AssetPipeline::CookRecord& record, std::size_t current, std::size_t)
    {
        if (current != 1)
        {
            return;
        }
        for (const std::string& target : swapTargets)
        {
            const fs::path path = fixture.source / target;
            std::error_code swapError;
            if (target == record.path || fs::symlink_status(path, swapError).type() != fs::file_type::regular)
            {
                continue;
            }
            fs::remove(path, swapError);
            fs::create_symlink(fixture.canaryFile, path, swapError);
            swapped = true;
        }
    };
    const auto cooked = Spark::AssetPipeline::CookAssets(request);
    CheckNoCanary(fixture);
    if (HasStagingSibling(fixture))
    {
        InvariantFailure("a cook left a staging directory");
    }
    if (!cooked.Succeeded())
    {
        const bool documented =
            (nonUtf8 && cooked.error.find("portable UTF-8") != std::string::npos) ||
            (swapped && cooked.error.find("changed while cooking") != std::string::npos) ||
            (manifestClash && cooked.error.find("conflicts with a cooked asset") != std::string::npos);
        if (!documented)
        {
            InvariantFailure("a cook failed for no documented reason that applies to the tree");
        }
        if (fs::exists(fixture.output, ec))
        {
            InvariantFailure("a failed cook published output");
        }
        return 0;
    }
    if (nonUtf8 || swapped || manifestClash)
    {
        InvariantFailure("a cook succeeded although a documented refusal applies");
    }
    CheckRecords(cooked, files);
    CheckPublishedOutput(fixture, cooked, files);

    const auto again = Spark::AssetPipeline::CookAssets({fixture.source, fixture.output, {}, false});
    if (!again.Succeeded() || again.updatedCount != 0 || again.unchangedCount != files.size() ||
        again.manifestSha256 != cooked.manifestSha256)
        InvariantFailure("re-cooking the unchanged tree changed something");
    CheckRecords(again, files);
    CheckPublishedOutput(fixture, again, files);
    return 0;
}
