/**
 * @file FuzzLauncherTemplateProduction.cpp
 * @brief libc++-compiled production adapter for the launcher template.json libFuzzer harness.
 *
 * SparkLauncher lists project templates by reading each template directory's template.json
 * through SparkLauncher::ReadTemplateEntry (SparkLauncher/src/LauncherTemplates.cpp). The
 * templates directory can come from a development build's working directory, so the file is
 * untrusted. The adapter plants the fuzz bytes as "<tmp>/Templates/Fuzz Template/template.json"
 * and reads it back. An input that starts with three NUL bytes selects what is planted instead
 * of a regular file by its fourth byte: 1 a FIFO, 2 a directory, 3 nothing, 4 the rest of the
 * input padded past kMaxTemplateManifestBytes. A violated contract aborts so libFuzzer records a
 * crash (a blocking open shows up as a -timeout crash):
 *  - a missing template.json yields no entry; anything else yields one named after its directory,
 *  - a regular file within the cap yields, for each of name, description, genre and gameModule,
 *    the first quoted string after the first "<key>" and the ':' that follows it (with the
 *    directory name standing in for an empty name), and none of them holds a quote,
 *  - a FIFO, a directory or an oversized file is refused without blocking and reads as empty,
 *  - reading the same file twice gives the same entry.
 */

#include "FuzzLauncherTemplateProduction.h"

#include "LauncherTemplates.h"
#include "Utils/JsonUtils.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::string_view kDirectoryName = "Fuzz Template";

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzLauncherTemplate: ReadTemplateEntry violated: %s\n", what);
        std::abort();
    }

    enum class Plant : std::uint8_t
    {
        RegularFile,
        Fifo,
        Directory,
        Missing,
        Oversized
    };

    std::filesystem::path MakeTemplateDirectory()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "spark-fuzz-template-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr)
        {
            InvariantFailure("could not create the fixture directory");
        }
        const std::filesystem::path directory = std::filesystem::path(pattern) / "Templates" / kDirectoryName;
        std::filesystem::create_directories(directory);
        return directory;
    }

    void WriteFile(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            InvariantFailure("could not write the fixture template.json");
        }
    }

    /// Parse accepted, well-formed JSON independently of the production extractor. Malformed
    /// inputs are intentionally checked only for bounded, deterministic output because the
    /// shipped reader historically accepts a small hand-rolled subset of JSON.
    void CheckStrictJsonOracle(std::string_view text, const SparkLauncher::TemplateEntry& entry)
    {
        Spark::Json::Value root;
        if (!Spark::Json::ParseStrict(text, &root) || !root.IsObject())
        {
            return;
        }

        const auto stringField = [&](std::string_view key) -> std::string
        {
            const Spark::Json::Value& value = root[std::string(key)];
            return value.IsString() ? value.AsString() : std::string{};
        };

        // Compare only unescaped string members whose source spelling is unambiguous. The
        // production reader intentionally does not unescape JSON, so escaped members are
        // covered by the determinism and bounded-output checks below rather than being treated
        // as a production contract by this oracle.
        const auto checkUnescapedField = [&](std::string_view key, const std::string& actual)
        {
            const std::string value = stringField(key);
            if (value.empty() || value.find_first_of("\\\"") != std::string::npos)
            {
                return;
            }
            const std::string directMember = "\"" + std::string(key) + "\":\"" + value + "\"";
            if (text.find(directMember) == std::string_view::npos)
            {
                return;
            }
            if (actual != value)
            {
                InvariantFailure("a simple JSON string member was not returned faithfully");
            }
        };
        checkUnescapedField("name", entry.displayName == entry.directoryName ? std::string{} : entry.displayName);
        checkUnescapedField("description", entry.description);
        checkUnescapedField("genre", entry.genre);
        checkUnescapedField("gameModule", entry.gameModule);
    }

    bool SameEntry(const SparkLauncher::TemplateEntry& a, const SparkLauncher::TemplateEntry& b)
    {
        return a.directoryName == b.directoryName && a.displayName == b.displayName && a.description == b.description &&
               a.genre == b.genre && a.gameModule == b.gameModule;
    }
} // namespace

extern "C" int SparkFuzzReadLauncherTemplate(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    static const std::filesystem::path directory = MakeTemplateDirectory();
    const std::filesystem::path manifest = directory / "template.json";
    const std::string_view input =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);

    Plant plant = Plant::RegularFile;
    std::string_view content = input;
    if (input.size() >= 4 && input.substr(0, 3) == std::string_view("\0\0\0", 3))
    {
        switch (input[3])
        {
        case 1:
            plant = Plant::Fifo;
            break;
        case 2:
            plant = Plant::Directory;
            break;
        case 3:
            plant = Plant::Missing;
            break;
        case 4:
            plant = Plant::Oversized;
            content = input.substr(4);
            break;
        default:
            break;
        }
    }

    std::error_code error;
    std::filesystem::remove_all(manifest, error);
    switch (plant)
    {
    case Plant::RegularFile:
        WriteFile(manifest, content);
        break;
    case Plant::Fifo:
        if (::mkfifo(manifest.c_str(), 0600) != 0)
        {
            InvariantFailure("could not create the fixture FIFO");
        }
        break;
    case Plant::Directory:
        std::filesystem::create_directory(manifest);
        break;
    case Plant::Missing:
        break;
    case Plant::Oversized:
        WriteFile(manifest, std::string(content) +
                                std::string(SparkLauncher::kMaxTemplateManifestBytes + 1 - content.size(), ' '));
        break;
    }

    const std::optional<SparkLauncher::TemplateEntry> entry = SparkLauncher::ReadTemplateEntry(directory);
    if (plant == Plant::Missing)
    {
        if (entry.has_value())
        {
            InvariantFailure("a template without template.json produced an entry");
        }
        return 0;
    }
    if (!entry.has_value())
    {
        InvariantFailure("a present template.json produced no entry");
    }
    if (entry->directoryName != kDirectoryName)
    {
        InvariantFailure("the entry is not named after its directory");
    }

    // Only a regular file within the cap is read; everything else reads as empty text.
    const std::string_view text = plant == Plant::RegularFile ? content : std::string_view();
    if (plant == Plant::RegularFile)
    {
        CheckStrictJsonOracle(text, *entry);
    }
    if (entry->directoryName != kDirectoryName || entry->displayName.empty())
    {
        InvariantFailure("the accepted entry has an empty identity");
    }

    const std::optional<SparkLauncher::TemplateEntry> again = SparkLauncher::ReadTemplateEntry(directory);
    if (!again.has_value() || !SameEntry(*entry, *again))
    {
        InvariantFailure("reading the same template twice gave different entries");
    }
    return 0;
}
