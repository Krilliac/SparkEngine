/**
 * @file CrashReportUploader.h
 * @brief GitHub crash report upload functionality (extracted from CrashHandler)
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides functions to upload crash reports as GitHub Issues, optionally
 * attaching compressed dump files as release assets. Requires libcurl
 * (SPARK_HAS_CURL) at compile time.
 *
 * @see CrashHandler, CrashConfig
 */

#pragma once

#include <string>

struct CrashConfig;

/**
 * @brief Create a GitHub Issue containing the crash log and optional dump file
 *
 * Creates a new GitHub Issue on the repository specified in @p cfg, with the
 * crash log embedded as markdown. If CrashConfig::githubAttachDump is true
 * and @p zipPath points to an existing file, the zip is uploaded as a draft
 * release asset and linked from the issue body.
 *
 * @param cfg        Crash configuration (must have githubRepo and githubToken set)
 * @param logContent The crash log text to embed in the issue body
 * @param zipPath    Path to the compressed crash dump file (empty = no attachment)
 * @return true if the GitHub Issue was created successfully
 */
bool UploadCrashToGitHub(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath);

/**
 * @brief Upload a file to a remote URL via HTTP multipart/form-data
 *
 * Sends the file at @p filePath as a multipart form field named @p field
 * to the given @p url using libcurl.
 *
 * @param cfg      Crash configuration (used for connection timeout)
 * @param url      The remote URL to upload to
 * @param filePath Path to the file to upload
 * @param field    The form field name for the file
 * @return true if the upload completed successfully (HTTP 2xx)
 */
bool UploadCrashFile(const CrashConfig& cfg, const std::string& url, const std::string& filePath,
                     const std::string& field);
