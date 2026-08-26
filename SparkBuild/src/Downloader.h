#pragma once

#include "Platform.h"
#include <string>
#include <functional>

namespace SparkBuild
{

    using DownloadProgressCallback = std::function<void(size_t bytesDownloaded, size_t totalBytes)>;

    class Downloader
    {
      public:
        // Download a file from a URL to a local path.
        static bool DownloadFile(const std::string& url, const std::string& outputPath,
                                 DownloadProgressCallback progress = nullptr);

        // Extract a ZIP file to a directory.
        static bool ExtractZip(const std::string& zipPath, const std::string& destDir);

        // Download and extract a ZIP in one step.
        static bool DownloadAndExtract(const std::string& url, const std::string& destDir,
                                       DownloadProgressCallback progress = nullptr);

        // Atomically reserve a unique .zip file in the system temp directory.
        // The caller owns the returned file and must remove it when finished.
        // Returns an empty string when no file can be reserved.
        static std::string ReserveTempDownloadPath();

        // Get the system temp directory
        static std::string GetTempDir();
    };

} // namespace SparkBuild
