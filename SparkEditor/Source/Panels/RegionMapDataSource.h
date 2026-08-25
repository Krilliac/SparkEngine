/**
 * @file RegionMapDataSource.h
 * @brief Safe Terrafront continent-to-region-map discovery for SparkEditor.
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SparkEditor
{
    struct RegionMapDataSource
    {
        int mapId = -1;
        std::string key;
        std::string name;
        std::string regionsFile;
        std::filesystem::path dataPath;
    };

    /// Only one plain JSON filename is accepted; absolute paths, directories,
    /// traversal, alternate extensions, and platform separators are rejected.
    bool IsSafeRegionMapFileName(std::string_view value);

    /// Reads continents.json from dataDirectory and returns every continent
    /// with a safe, existing region lattice. Sanctuary entries are skipped.
    bool LoadRegionMapDataSources(const std::filesystem::path& dataDirectory,
                                  std::vector<RegionMapDataSource>& outSources, std::string& outError);

    /// Strict-JSON-validates and atomically replaces one region-map document.
    /// An existing destination is first copied to a sibling `.bak` through the
    /// same temporary-file + atomic-replace path. A failed validation or backup
    /// leaves the destination untouched.
    bool WriteRegionMapDocumentAtomically(const std::filesystem::path& destination, std::string_view document,
                                          std::string& outError);
} // namespace SparkEditor
