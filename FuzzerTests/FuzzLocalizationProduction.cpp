/**
 * @file FuzzLocalizationProduction.cpp
 * @brief libc++ production adapter for StringTable::LoadFromFile.
 */

#include "FuzzLocalizationProduction.h"

#include "Engine/Localization/LocalizationSystem.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    [[noreturn]] void InvariantFailure(const char* message)
    {
        std::fprintf(stderr, "SparkFuzzLocalization: %s\n", message);
        std::abort();
    }
} // namespace

extern "C" int SparkFuzzLoadLocalization(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }

    const std::string content = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);
    const auto threadTag = std::hash<std::thread::id>{}(std::this_thread::get_id());
#if defined(_WIN32)
    const auto processTag = static_cast<unsigned long long>(_getpid());
#else
    const auto processTag = static_cast<unsigned long long>(getpid());
#endif
    const std::filesystem::path fixturePath =
        std::filesystem::temp_directory_path() /
        ("spark-fuzz-localization-" + std::to_string(processTag) + "-" + std::to_string(threadTag) + ".json");
    std::ofstream file(fixturePath, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        InvariantFailure("cannot create bounded fixture file");
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();

    Spark::StringTable table;
    table.SetEntry("__baseline__", "preserved");
    const bool accepted = table.LoadFromFile(fixturePath.string());
    if (!accepted)
    {
        if (table.GetEntry("__baseline__") != "preserved" || table.GetEntryCount() != 1)
        {
            InvariantFailure("rejected catalog changed the existing table");
        }
    }
    else
    {
        // Entries merge over the existing table, so an accepted catalog either adds a key or
        // overwrites the baseline one.
        if (table.GetEntryCount() < 2 && table.GetEntry("__baseline__") == "preserved")
        {
            InvariantFailure("accepted catalog added no entries");
        }
        // Independent model: an accepted catalog is one complete JSON object (optional UTF-8 BOM).
        const size_t bomBytes = content.starts_with("\xEF\xBB\xBF") ? 3 : 0;
        const size_t first = content.find_first_not_of(" \t\n\r\f\v", bomBytes);
        const size_t last = content.find_last_not_of(" \t\n\r\f\v");
        if (first == std::string::npos || content[first] != '{' || last == std::string::npos || content[last] != '}')
        {
            InvariantFailure("accepted catalog is not a complete JSON object");
        }
        for (const std::string& key : table.GetAllKeys())
        {
            if (key != "__baseline__" && (key.size() > size || table.GetEntry(key).size() > size))
            {
                InvariantFailure("accepted entry exceeds the input budget");
            }
        }
    }

    std::error_code removeError;
    std::filesystem::remove(fixturePath, removeError);
    return 0;
}
