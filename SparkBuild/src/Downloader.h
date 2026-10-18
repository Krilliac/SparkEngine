#pragma once

#include "Platform.h"
#include <string>
#include <functional>

namespace SparkBuild
{

    enum class ArchiveFormat
    {
        Unknown,
        Zip,
        GzipTar
    };

    using DownloadProgressCallback = std::function<void(size_t bytesDownloaded, size_t totalBytes)>;

    class Downloader
    {
      public:
        // Download a file from a URL to a local path.
        static bool DownloadFile(const std::string& url, const std::string& outputPath,
                                 DownloadProgressCallback progress = nullptr);

        // Detect the archive container from its leading magic bytes
        // (PK\x03\x04 = ZIP, 1F 8B = gzip-compressed tar), never from its name.
        static ArchiveFormat DetectArchiveFormat(const std::string& archivePath);

        // The format a URL or file name promises by suffix (.zip, .tar.gz, .tgz).
        // Unknown when the name carries no recognised suffix.
        static ArchiveFormat ArchiveFormatFromName(const std::string& nameOrUrl);

        // Verify an already-downloaded archive and extract it without ever
        // overwriting destDir content. The SHA-256 must match and the detected
        // content must equal expectedFormat (Unknown is always refused). Members
        // are extracted synchronously into a unique staging directory inside
        // destDir (same volume even when destDir is a symlink or junction). A
        // ZIP extraction must produce exactly the listed members; the staged
        // tree must be self-contained (no absolute or '..' members, no symlink
        // that is absolute, climbs out, or resolves outside it, no special
        // files), and its top-level entries are then moved into destDir only if
        // none of them already exists. Staging is removed on every path; on
        // failure destDir is left unchanged (and removed if this call created it).
        static bool ExtractVerifiedArchive(const std::string& archivePath, const std::string& destDir,
                                           const std::string& expectedSha256, ArchiveFormat expectedFormat);

        // Download, verify, and extract an archive in one step. The expected
        // SHA-256 is mandatory so callers cannot silently skip integrity checks.
        // The URL suffix names the expected format; an unrecognised suffix fails
        // before any download.
        static bool DownloadAndExtract(const std::string& url, const std::string& destDir,
                                       const std::string& expectedSha256, DownloadProgressCallback progress = nullptr);

        // Atomically reserve a unique archive file in the system temp directory.
        // The caller owns the returned file and must remove it when finished.
        // Returns an empty string when no file can be reserved.
        static std::string ReserveTempDownloadPath(const std::string& archiveSuffix = ".zip");

        // Get the system temp directory
        static std::string GetTempDir();
    };

} // namespace SparkBuild
