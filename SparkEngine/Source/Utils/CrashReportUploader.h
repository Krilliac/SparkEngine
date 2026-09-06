/**
 * @file CrashReportUploader.h
 * @brief Multi-backend crash report upload (GitHub, Dropbox, FTP, HTTP, proxy)
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides functions to upload crash reports via multiple backends:
 * - **GitHub Issues** — direct API with PAT (dev builds)
 * - **Proxy server** — relays to GitHub, token stays server-side (release builds)
 * - **Dropbox** — shared file request link or API upload
 * - **FTP/FTPS** — standard FTP upload via libcurl
 * - **Generic HTTP** — multipart POST to any endpoint
 *
 * The top-level UploadCrashReport() auto-detects the backend from the URL.
 * Requires libcurl (SPARK_HAS_CURL) at compile time.
 *
 * @see CrashHandler, CrashConfig
 */

#pragma once

#include <string>

struct CrashConfig;

/**
 * @brief Auto-detect backend and upload crash report
 *
 * Examines CrashConfig fields to choose the upload method:
 *   1. proxyURL set          → proxy server
 *   2. githubRepo + token    → GitHub Issues (direct)
 *   3. uploadURL detected    → auto-detect from URL prefix:
 *      - dropbox.com / dbx:// → Dropbox
 *      - ftp:// / ftps://     → FTP
 *      - http(s)://           → generic HTTP POST
 *
 * @param cfg        Crash configuration
 * @param logContent The crash log text
 * @param zipPath    Path to the compressed crash dump file (empty = no attachment)
 * @return true if upload succeeded via any backend
 */
bool UploadCrashReport(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath);

/**
 * @brief Create a GitHub Issue containing the crash log and optional dump file
 *
 * Deduplication: computes a hash of the top stack frames and searches open
 * issues for a matching tag. If found, adds a comment instead of creating
 * a duplicate issue.
 *
 * @param cfg        Crash configuration (must have githubRepo and githubToken set)
 * @param logContent The crash log text to embed in the issue body
 * @param zipPath    Path to the compressed crash dump file (empty = no attachment)
 * @return true if the GitHub Issue was created (or comment added) successfully
 */
bool UploadCrashToGitHub(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath);

/**
 * @brief Upload crash report to a proxy server (release builds)
 *
 * Sends the crash log and optional dump file to a proxy endpoint via
 * multipart/form-data POST. The proxy holds the GitHub token and creates
 * the Issue server-side.
 *
 * @param cfg        Crash configuration (must have proxyURL set)
 * @param logContent The crash log text
 * @param zipPath    Path to the compressed crash dump file (empty = no attachment)
 * @return true if the proxy accepted the upload (HTTP 2xx)
 */
bool UploadCrashToProxy(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath);

/**
 * @brief Upload crash dump to a Dropbox shared link or file request
 *
 * Supports Dropbox file request URLs (dropbox.com/request/...) and
 * direct API uploads (dbx:// scheme, requires token in uploadURL).
 *
 * @param cfg        Crash configuration (uploadURL must be a Dropbox URL)
 * @param logContent The crash log text (uploaded as .txt alongside the zip)
 * @param zipPath    Path to the compressed crash dump file
 * @return true if the upload completed successfully
 */
bool UploadCrashToDropbox(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath);

/**
 * @brief Upload crash dump via FTP/FTPS
 *
 * @param cfg        Crash configuration (uploadURL must be ftp:// or ftps://)
 * @param zipPath    Path to the compressed crash dump file
 * @return true if the upload completed successfully
 */
bool UploadCrashToFTP(const CrashConfig& cfg, const std::string& zipPath);

/**
 * @brief Send crash report via SMTP email
 *
 * Uses libcurl SMTP support to email the crash log (and optional zip)
 * directly to the configured recipient. Requires smtp:// or smtps:// URL
 * in uploadURL and SmtpUser/SmtpPass/EmailTo configured.
 *
 * @param cfg        Crash configuration (needs uploadURL=smtp://.., smtpUser, emailTo)
 * @param logContent The crash log text (sent as email body)
 * @param zipPath    Path to the compressed crash dump file (attached if present)
 * @return true if the email was sent successfully
 */
bool UploadCrashToEmail(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath);

/**
 * @brief Upload a file to a remote URL via HTTP multipart/form-data
 *
 * @param cfg      Crash configuration (used for connection timeout)
 * @param url      The remote URL to upload to
 * @param filePath Path to the file to upload
 * @param field    The form field name for the file
 * @return true if the upload completed successfully (HTTP 2xx)
 */
bool UploadCrashFile(const CrashConfig& cfg, const std::string& url, const std::string& filePath,
                     const std::string& field);

/**
 * @brief Marker every engine stack-trace producer puts on a frame line
 *
 * CrashHandler's SymStackTrace() (Windows) and CaptureStackTraceString()
 * (POSIX) write "  FRAME <symbol> +0x<displacement>", and ComputeStackHash
 * keys on this marker rather than guessing at a format. Producer and consumer
 * share one constant so they cannot drift apart: before this existed, no line
 * the engine wrote matched the parser and every crash hashed to "".
 */
inline constexpr const char* kStackFrameMarker = "FRAME ";
inline constexpr const wchar_t* kStackFrameMarkerW = L"FRAME ";

/**
 * @brief Compute a short hex hash of the top stack frames from a crash log
 *
 * Extracts function names from the first @p maxFrames stack frames and
 * hashes them with FNV-1a. Used for deduplication.
 *
 * @param logContent The full crash log text
 * @param maxFrames  Maximum number of stack frames to include (default: 5)
 * @return Hex string hash, or empty string if no stack frames found
 */
std::string ComputeStackHash(const std::string& logContent, int maxFrames = 5);

/**
 * @brief Check if upload is rate-limited (60s cooldown between uploads)
 * @return true if rate-limited (should skip upload)
 */
bool IsCrashUploadRateLimited();
