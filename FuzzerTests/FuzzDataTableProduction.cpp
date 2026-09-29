/** @brief libc++-compiled production adapter for DataTable fuzzing. */
#include "FuzzDataTableProduction.h"

#include "Engine/DataTable/DataTableSystem.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#include <string>

namespace
{
    constexpr std::size_t kMaxInputBytes = 8u * 1024u * 1024u + 1u;
    std::atomic<std::uint64_t> s_fileCounter{0};

    std::uint64_t ProcessId()
    {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(_getpid());
#else
        return static_cast<std::uint64_t>(getpid());
#endif
    }
    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzDataTable: %s\n", what);
        std::abort();
    }

    void CheckRoundTrip(const Spark::Data::DataTable& table, bool json)
    {
        const std::string encoded = json ? table.SaveToJSON() : table.SaveToCSV();
        Spark::Data::DataTable reloaded;
        const bool accepted = json ? reloaded.LoadFromJSON(encoded) : reloaded.LoadFromCSV(encoded);
        if (!accepted || reloaded.GetRowCount() != table.GetRowCount() ||
            reloaded.GetColumnCount() != table.GetColumnCount())
            InvariantFailure("accepted table does not survive its production round trip");
        const std::string reencoded = json ? reloaded.SaveToJSON() : reloaded.SaveToCSV();
        if (reencoded != encoded)
            InvariantFailure("accepted table changed cell data during its production round trip");
    }
} // namespace

extern "C" int SparkFuzzParseDataTable(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    const std::uint8_t selector = size == 0 ? 0 : data[0];
    const bool json = selector == '[' || selector == ' ' || selector == '\n' || selector == '\r' || selector == '\t';
    const std::string content = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);
    Spark::Data::DataTable table;
    const bool accepted = json ? table.LoadFromJSON(content) : table.LoadFromCSV(content);
    if (accepted)
        CheckRoundTrip(table, json);

    const auto suffix = s_fileCounter.fetch_add(1, std::memory_order_relaxed);
    std::string fileName = "spark-fuzz-datatable-";
    fileName += std::to_string(ProcessId());
    fileName += "-";
    fileName += std::to_string(suffix);
    fileName += json ? ".json" : ".csv";
    const std::filesystem::path path = std::filesystem::temp_directory_path() / fileName;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (file)
        {
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            file.close();
            auto& registry = Spark::Data::DataTableRegistry::GetInstance();
            registry.Initialize();
            const bool fileAccepted = registry.LoadTableFromFile("fuzz", path.string());
            if (fileAccepted != accepted)
                InvariantFailure("registry file loader disagrees with in-memory loader");
            registry.Shutdown();
        }
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return 0;
}
