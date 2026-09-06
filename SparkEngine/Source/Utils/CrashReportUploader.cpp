/**
 * @file CrashReportUploader.cpp
 * @brief GitHub crash report upload implementation (extracted from CrashHandler)
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains the GitHub Issue creation, proxy upload, deduplication, and file
 * upload logic. All functions are no-ops when SPARK_HAS_CURL is not defined.
 *
 * @see CrashReportUploader.h, CrashHandler.h
 */

#include "Utils/CrashReportUploader.h"
#include "Utils/CrashHandler.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_CURL
#include <curl/curl.h>
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <ctime>
#include <chrono>
#include <atomic>
#include <cstdint>

// ============================================================================
// Rate limiting
// ============================================================================

static std::atomic<int64_t> g_lastUploadEpoch{0};
static constexpr int64_t kRateLimitSeconds = 60;

bool IsCrashUploadRateLimited()
{
    auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t last = g_lastUploadEpoch.load(std::memory_order_relaxed);
    if (last > 0 && (now - last) < kRateLimitSeconds)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core,
                       "CrashReportUploader: Rate-limited — last upload was %lld seconds ago (limit: %lld)",
                       static_cast<long long>(now - last), static_cast<long long>(kRateLimitSeconds));
        return true;
    }
    return false;
}

static void MarkUploadTime()
{
    auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    g_lastUploadEpoch.store(now, std::memory_order_relaxed);
}

// ============================================================================
// Stack hash (FNV-1a on top N frame function names)
// ============================================================================

std::string ComputeStackHash(const std::string& logContent, int maxFrames)
{
    // Look for lines containing common stack frame patterns:
    //   "0x... FunctionName" or "#N 0x... in FunctionName" or "FunctionName + 0x..."
    // We extract the function/symbol portion and hash it.
    std::istringstream iss(logContent);
    std::string line;
    std::string frameData;
    int frameCount = 0;

    while (std::getline(iss, line) && frameCount < maxFrames)
    {
        // Skip empty or header lines
        if (line.size() < 4)
            continue;

        // Detect stack frame lines by common patterns
        bool isFrame = false;

        // Pattern: "  FRAME <symbol> +0x<disp>" — the marker the engine's own
        // producers emit (CrashHandler SymStackTrace / CaptureStackTraceString).
        // It is checked first and deliberately explicit: the heuristics below
        // matched no line this engine has ever written, which is why every
        // Windows crash used to hash to "" and open a fresh GitHub issue.
        if (line.find(kStackFrameMarker) != std::string::npos)
            isFrame = true;
        // Pattern: "  #N " (GCC/backtrace style)
        else if (line.find("  #") != std::string::npos)
            isFrame = true;
        // Pattern: lines starting with "0x" (Windows symbol output)
        else if (line.starts_with("0x") || line.starts_with("  0x"))
            isFrame = true;
        // Pattern: "Frame N:" or "[N]"
        else if (line.find("Frame ") != std::string::npos || line.find("SymStackTrace") != std::string::npos)
            isFrame = true;
        // Pattern: contains " + 0x" (module+offset style)
        else if (line.find(" + 0x") != std::string::npos)
            isFrame = true;

        if (isFrame)
        {
            // Strip addresses (volatile between runs) — remove 0x[hex]+ patterns
            // and bare hex/digit sequences, keep function names and separators
            std::string cleaned;
            size_t i = 0;
            while (i < line.size())
            {
                // Skip 0x[hex]+ address patterns
                if (i + 1 < line.size() && line[i] == '0' && (line[i + 1] == 'x' || line[i + 1] == 'X'))
                {
                    i += 2;
                    while (i < line.size() &&
                           ((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f') ||
                            (line[i] >= 'A' && line[i] <= 'F')))
                        ++i;
                    if (!cleaned.empty() && cleaned.back() != ' ')
                        cleaned += ' ';
                    continue;
                }
                // Skip standalone digit sequences
                if (line[i] >= '0' && line[i] <= '9')
                {
                    while (i < line.size() && line[i] >= '0' && line[i] <= '9')
                        ++i;
                    if (!cleaned.empty() && cleaned.back() != ' ')
                        cleaned += ' ';
                    continue;
                }
                // Keep alpha, underscore, colon (function names)
                if ((line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= 'a' && line[i] <= 'z') || line[i] == '_' ||
                    line[i] == ':')
                {
                    cleaned += line[i];
                }
                else if (!cleaned.empty() && cleaned.back() != ' ')
                {
                    cleaned += ' ';
                }
                ++i;
            }
            frameData += cleaned + "\n";
            ++frameCount;
        }
    }

    if (frameData.empty())
        return "";

    // FNV-1a 32-bit hash
    uint32_t hash = 0x811c9dc5u;
    for (char c : frameData)
    {
        hash ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
        hash *= 0x01000193u;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", hash);
    return buf;
}

// ============================================================================
// JSON helper
// ============================================================================

// Escape a string for safe embedding inside a JSON string value.
// Handles control characters, quotes, and backslashes per RFC 8259.
static std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 32);
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char hex[8];
                snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned char>(c));
                out += hex;
            }
            else
            {
                out += c;
            }
            break;
        }
    }
    return out;
}

#ifdef SPARK_HAS_CURL

// ============================================================================
// libcurl helpers
// ============================================================================

// libcurl write callback — appends response body to a std::string
static size_t GitHubWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* response = static_cast<std::string*>(userdata);
    size_t bytes = size * nmemb;
    response->append(ptr, bytes);
    return bytes;
}

// Check HTTP response code, log on failure. Returns true if 2xx.
static bool CheckHttpResponse(CURL* c, const char* operation)
{
    long httpCode = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpCode);
    if (httpCode >= 200 && httpCode < 300)
        return true;
    SPARK_LOG_ERROR(Spark::LogCategory::Core, "CrashReportUploader: %s failed with HTTP %ld", operation, httpCode);
    return false;
}

// ============================================================================
// Transfer bounds
// ============================================================================

/// Ceiling on one complete crash-report request.
static constexpr long kUploadTimeoutSeconds = 30;
/// A transfer moving fewer than this many bytes/s for kUploadStallSeconds is dead.
static constexpr long kUploadLowSpeedBytesPerSecond = 512;
static constexpr long kUploadStallSeconds = 15;
/// Largest crash archive that is read into the crashing process's memory and posted.
static constexpr uintmax_t kMaxAttachmentBytes = 32ull * 1024 * 1024;

// Bound a libcurl handle in time, not only at connect. The upload runs on the
// crashing thread inside HandleCrashInternal: with CURLOPT_CONNECTTIMEOUT alone,
// an endpoint that accepts the connection and then stalls leaves
// curl_easy_perform blocked forever, so the process neither reports nor exits.
static void ApplyTransferBounds(CURL* handle, const CrashConfig& cfg)
{
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(cfg.connectTimeoutSeconds));
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, kUploadTimeoutSeconds);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, kUploadLowSpeedBytesPerSecond);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, kUploadStallSeconds);
}

// Build standard GitHub API headers. Caller must free with curl_slist_free_all().
static struct curl_slist* BuildGitHubHeaders(const std::string& token, const char* contentType)
{
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, contentType);
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    headers = curl_slist_append(headers, "User-Agent: SparkEngine-CrashHandler/1.0");
    return headers;
}

// Extract a JSON string value by key (simple string search — avoids JSON lib dependency)
static std::string ExtractJsonString(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return "";
    pos += needle.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos)
        return "";
    return json.substr(pos, end - pos);
}

// ============================================================================
// Deduplication — search open issues for matching stack hash
// ============================================================================

// Search open issues for a matching [HASH:xxxx] tag in the title.
// Returns the issue number if found, or 0 if no match.
static int FindDuplicateIssue(const CrashConfig& cfg, const std::string& stackHash)
{
    if (stackHash.empty())
        return 0;

    std::string searchUrl = "https://api.github.com/repos/" + cfg.githubRepo + "/issues?labels=" + cfg.githubLabels +
                            "&state=open&per_page=100";

    std::string response;
    CURL* c = curl_easy_init();
    if (!c)
        return 0;

    struct curl_slist* headers = BuildGitHubHeaders(cfg.githubToken, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, searchUrl.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    ApplyTransferBounds(c, cfg);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

    CURLcode res = curl_easy_perform(c);
    bool httpOk = CheckHttpResponse(c, "issue search");
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || !httpOk)
        return 0;

    // Search for "[HASH:xxxx]" in issue titles within the JSON array
    std::string hashTag = "[HASH:" + stackHash + "]";
    size_t tagPos = response.find(hashTag);
    if (tagPos == std::string::npos)
        return 0;

    // Walk backwards to find the "number": field for this issue object
    // Issues in the array have "number":N before the "title" field
    std::string numberNeedle = "\"number\":";
    size_t searchStart = (tagPos > 500) ? tagPos - 500 : 0;
    std::string region = response.substr(searchStart, tagPos - searchStart);
    size_t numPos = region.rfind(numberNeedle);
    if (numPos == std::string::npos)
        return 0;

    numPos += numberNeedle.size();
    int issueNum = 0;
    while (numPos < region.size() && region[numPos] >= '0' && region[numPos] <= '9')
    {
        issueNum = issueNum * 10 + (region[numPos] - '0');
        ++numPos;
    }
    return issueNum;
}

// Add a comment to an existing issue with the new crash occurrence
static bool AddDuplicateComment(const CrashConfig& cfg, int issueNumber, const std::string& logContent)
{
    std::string commentUrl =
        "https://api.github.com/repos/" + cfg.githubRepo + "/issues/" + std::to_string(issueNumber) + "/comments";

    std::string body = "## Additional Crash Occurrence\\n\\n"
                       "Another instance of this crash was detected.\\n\\n"
                       "### Crash Log\\n\\n"
                       "```\\n" +
                       JsonEscape(logContent) + "\\n```";

    std::string json = "{\"body\":\"" + body + "\"}";

    std::string response;
    CURL* c = curl_easy_init();
    if (!c)
        return false;

    struct curl_slist* headers = BuildGitHubHeaders(cfg.githubToken, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, commentUrl.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    ApplyTransferBounds(c, cfg);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

    CURLcode res = curl_easy_perform(c);
    bool httpOk = CheckHttpResponse(c, "add duplicate comment");
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (res == CURLE_OK && httpOk)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Added crash occurrence comment to issue #%d",
                       issueNumber);
        return true;
    }
    return false;
}

// ============================================================================
// GitHub Issue upload (direct API — dev builds with PAT)
// ============================================================================

bool UploadCrashToGitHub(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath)
{
    if (cfg.githubRepo.empty() || cfg.githubToken.empty())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core,
                       "CrashReportUploader: GitHub repo or token not configured, skipping upload");
        return false;
    }

    if (IsCrashUploadRateLimited())
        return false;

    SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Uploading crash report to GitHub repo: %s",
                   cfg.githubRepo.c_str());

    // ---- Compute stack hash for deduplication ----
    std::string stackHash = ComputeStackHash(logContent);

    // ---- Check for duplicate issues ----
    if (!stackHash.empty())
    {
        int existingIssue = FindDuplicateIssue(cfg, stackHash);
        if (existingIssue > 0)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Found duplicate issue #%d, adding comment",
                           existingIssue);
            bool ok = AddDuplicateComment(cfg, existingIssue, logContent);
            if (ok)
                MarkUploadTime();
            return ok;
        }
    }

    // ---- Build issue body ----
    std::ostringstream body;
    body << "## Automated Crash Report\\n\\n";
    body << "This issue was automatically created by the SparkEngine crash handler.\\n\\n";
    body << "### Crash Log\\n\\n";
    body << "```\\n" << JsonEscape(logContent) << "\\n```\\n";

    // ---- Build issue title ----
    std::string title = "Crash Report";
    {
        std::istringstream iss(logContent);
        std::string line;
        while (std::getline(iss, line))
        {
            if (line.contains("ASSERTION FAILURE"))
            {
                title = "Assertion Failure";
                break;
            }
            if (line.contains("CRASH DETECTED"))
            {
                title = "Crash Detected";
                break;
            }
            if (line.contains("SIGSEGV"))
            {
                title = "Crash: SIGSEGV (Segmentation fault)";
                break;
            }
            if (line.contains("SIGABRT"))
            {
                title = "Crash: SIGABRT (Abort)";
                break;
            }
            if (line.contains("SIGFPE"))
            {
                title = "Crash: SIGFPE (Floating point exception)";
                break;
            }
        }
    }

    // Prepend stack hash tag for deduplication
    if (!stackHash.empty())
        title = "[HASH:" + stackHash + "] " + title;

    // Append timestamp to title to keep issues distinct
    {
        time_t now = time(nullptr);
        struct tm t;
#ifdef SPARK_PLATFORM_WINDOWS
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        char timeBuf[32];
        snprintf(timeBuf, sizeof(timeBuf), " — %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        title += timeBuf;
    }

    // ---- Upload zip as release asset (optional) ----
    std::string assetLink;
    if (cfg.githubAttachDump && !zipPath.empty() && std::filesystem::exists(zipPath))
    {
        std::string tagName = "crash-dump-";
        {
            time_t now = time(nullptr);
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(now));
            tagName += buf;
        }

        std::string releaseJson = "{\"tag_name\":\"" + JsonEscape(tagName) +
                                  "\",\"name\":\"Crash Dump Upload\","
                                  "\"body\":\"Automated crash dump upload.\","
                                  "\"draft\":true,\"prerelease\":true}";

        std::string releaseUrl = "https://api.github.com/repos/" + cfg.githubRepo + "/releases";
        std::string releaseResponse;

        CURL* c = curl_easy_init();
        if (c)
        {
            struct curl_slist* headers = BuildGitHubHeaders(cfg.githubToken, "Content-Type: application/json");

            curl_easy_setopt(c, CURLOPT_URL, releaseUrl.c_str());
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, releaseJson.c_str());
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &releaseResponse);
            ApplyTransferBounds(c, cfg);
            curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

            CURLcode res = curl_easy_perform(c);
            bool httpOk = CheckHttpResponse(c, "create draft release");
            curl_slist_free_all(headers);
            curl_easy_cleanup(c);

            if (res != CURLE_OK || !httpOk)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "CrashReportUploader: Draft release creation failed, issue will be created without "
                               "dump attachment");
            }
            else
            {
                // Extract upload_url: "https://uploads.github.com/repos/.../releases/.../assets{?name,label}"
                std::string needle = "\"upload_url\":\"";
                size_t pos = releaseResponse.find(needle);
                if (pos != std::string::npos)
                {
                    pos += needle.size();
                    size_t end = releaseResponse.find('{', pos); // cut before {?name,label}
                    if (end == std::string::npos)
                        end = releaseResponse.find('"', pos);
                    std::string uploadUrl = releaseResponse.substr(pos, end - pos);

                    std::string filename = std::filesystem::path(zipPath).filename().string();
                    uploadUrl += "?name=" + filename;

                    // Read zip into memory
                    std::ifstream zipFile(zipPath, std::ios::binary | std::ios::ate);
                    if (zipFile.is_open() && static_cast<uintmax_t>(zipFile.tellg()) > kMaxAttachmentBytes)
                    {
                        // The buffer lives on the crashing process's heap and the
                        // POST is bounded by kUploadTimeoutSeconds; an oversized
                        // archive cannot make either budget, so refuse it here
                        // rather than fail slowly with the report already lost.
                        SPARK_LOG_WARN(Spark::LogCategory::Core,
                                       "CrashReportUploader: attachment %s is larger than the %llu byte limit — "
                                       "issue created without it",
                                       zipPath.c_str(), static_cast<unsigned long long>(kMaxAttachmentBytes));
                        zipFile.close();
                    }
                    else if (zipFile.is_open())
                    {
                        auto fileSize = zipFile.tellg();
                        zipFile.seekg(0, std::ios::beg);
                        std::vector<char> fileData(static_cast<size_t>(fileSize));
                        zipFile.read(fileData.data(), fileSize);
                        zipFile.close();

                        std::string assetResponse;
                        CURL* c2 = curl_easy_init();
                        if (c2)
                        {
                            struct curl_slist* h2 =
                                BuildGitHubHeaders(cfg.githubToken, "Content-Type: application/zip");

                            curl_easy_setopt(c2, CURLOPT_URL, uploadUrl.c_str());
                            curl_easy_setopt(c2, CURLOPT_HTTPHEADER, h2);
                            curl_easy_setopt(c2, CURLOPT_POSTFIELDS, fileData.data());
                            curl_easy_setopt(c2, CURLOPT_POSTFIELDSIZE, static_cast<long>(fileSize));
                            curl_easy_setopt(c2, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
                            curl_easy_setopt(c2, CURLOPT_WRITEDATA, &assetResponse);
                            ApplyTransferBounds(c2, cfg);
                            curl_easy_setopt(c2, CURLOPT_NOPROGRESS, 1L);

                            CURLcode res2 = curl_easy_perform(c2);
                            bool httpOk2 = CheckHttpResponse(c2, "upload release asset");
                            curl_slist_free_all(h2);
                            curl_easy_cleanup(c2);

                            if (res2 == CURLE_OK && httpOk2)
                            {
                                assetLink = ExtractJsonString(assetResponse, "browser_download_url");
                            }
                            else
                            {
                                SPARK_LOG_WARN(Spark::LogCategory::Core,
                                               "CrashReportUploader: Asset upload failed, issue will be created "
                                               "without dump link");
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Append asset link to body if available ----
    std::string fullBody = body.str();
    if (!assetLink.empty())
    {
        fullBody += "\\n### Crash Dump\\n\\n";
        fullBody += "[Download crash dump (.zip)](" + JsonEscape(assetLink) + ")\\n";
    }

    // ---- Parse labels into JSON array ----
    std::string labelsJson = "[";
    {
        std::istringstream labelStream(cfg.githubLabels);
        std::string label;
        bool first = true;
        while (std::getline(labelStream, label, ','))
        {
            size_t start = label.find_first_not_of(" \t");
            size_t end = label.find_last_not_of(" \t");
            if (start == std::string::npos)
                continue;
            label = label.substr(start, end - start + 1);
            if (!first)
                labelsJson += ",";
            labelsJson += "\"" + JsonEscape(label) + "\"";
            first = false;
        }
    }
    labelsJson += "]";

    // ---- Create the issue ----
    std::string issueJson =
        "{\"title\":\"" + JsonEscape(title) + "\",\"body\":\"" + fullBody + "\",\"labels\":" + labelsJson + "}";

    std::string issueUrl = "https://api.github.com/repos/" + cfg.githubRepo + "/issues";
    std::string issueResponse;

    CURL* c = curl_easy_init();
    if (!c)
        return false;

    struct curl_slist* headers = BuildGitHubHeaders(cfg.githubToken, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL, issueUrl.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, issueJson.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &issueResponse);
    ApplyTransferBounds(c, cfg);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

    CURLcode res = curl_easy_perform(c);
    bool httpOk = CheckHttpResponse(c, "create issue");
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    bool success = (res == CURLE_OK && httpOk && issueResponse.contains("\"id\""));
    if (success)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: GitHub issue created successfully");
        MarkUploadTime();
    }
    else
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "CrashReportUploader: Failed to create GitHub issue");
    }
    return success;
}

// ============================================================================
// Proxy upload (release builds — token lives server-side)
// ============================================================================

bool UploadCrashToProxy(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath)
{
    if (cfg.proxyURL.empty())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashReportUploader: Proxy URL not configured, skipping upload");
        return false;
    }

    if (IsCrashUploadRateLimited())
        return false;

    SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Uploading crash report to proxy: %s",
                   cfg.proxyURL.c_str());

    // Build title from crash log
    std::string title = "Crash Report";
    {
        std::istringstream iss(logContent);
        std::string line;
        while (std::getline(iss, line))
        {
            if (line.contains("ASSERTION FAILURE"))
            {
                title = "Assertion Failure";
                break;
            }
            if (line.contains("CRASH DETECTED"))
            {
                title = "Crash Detected";
                break;
            }
            if (line.contains("SIGSEGV"))
            {
                title = "Crash: SIGSEGV";
                break;
            }
            if (line.contains("SIGABRT"))
            {
                title = "Crash: SIGABRT";
                break;
            }
            if (line.contains("SIGFPE"))
            {
                title = "Crash: SIGFPE";
                break;
            }
        }
    }

    std::string stackHash = ComputeStackHash(logContent);

    CURL* c = curl_easy_init();
    if (!c)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "CrashReportUploader: Failed to initialize CURL for proxy upload");
        return false;
    }

    curl_mime* mime = curl_mime_init(c);

    // Text fields
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "log");
    curl_mime_data(part, logContent.c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "title");
    curl_mime_data(part, title.c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "stack_hash");
    curl_mime_data(part, stackHash.c_str(), CURL_ZERO_TERMINATED);

    // Dump file (optional)
    if (!zipPath.empty() && std::filesystem::exists(zipPath))
    {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "dump");
        curl_mime_filedata(part, zipPath.c_str());
    }

    std::string response;
    curl_easy_setopt(c, CURLOPT_URL, cfg.proxyURL.c_str());
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    ApplyTransferBounds(c, cfg);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

    CURLcode res = curl_easy_perform(c);
    bool httpOk = CheckHttpResponse(c, "proxy upload");
    curl_mime_free(mime);
    curl_easy_cleanup(c);

    bool success = (res == CURLE_OK && httpOk);
    if (success)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Proxy accepted crash report");
        MarkUploadTime();
    }
    else
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "CrashReportUploader: Proxy upload failed");
    }
    return success;
}

// ============================================================================
// Generic file upload
// ============================================================================

bool UploadCrashFile(const CrashConfig& cfg, const std::string& url, const std::string& filePath,
                     const std::string& field)
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Core, "CrashReportUploader: Uploading file '%s' to %s", filePath.c_str(),
                    url.c_str());
    CURL* c = curl_easy_init();
    if (!c)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "CrashReportUploader: Failed to initialize CURL for file upload");
        return false;
    }
    curl_mime* mime = curl_mime_init(c);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, field.c_str());
    curl_mime_filedata(part, filePath.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    ApplyTransferBounds(c, cfg);
    CURLcode res = curl_easy_perform(c);
    bool httpOk = CheckHttpResponse(c, "file upload");
    curl_mime_free(mime);
    curl_easy_cleanup(c);
    if (res != CURLE_OK)
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "CrashReportUploader: File upload failed with CURL error %d",
                        static_cast<int>(res));
    return (res == CURLE_OK && httpOk);
}

// ============================================================================
// Dropbox upload (shared file request link or API)
// ============================================================================

bool UploadCrashToDropbox(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath)
{
    if (cfg.uploadURL.empty())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashReportUploader: Dropbox URL not configured");
        return false;
    }

    if (IsCrashUploadRateLimited())
        return false;

    SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Uploading crash report to Dropbox");

    // Generate a timestamped filename
    char timeBuf[64];
    {
        time_t now = time(nullptr);
        struct tm t;
#ifdef SPARK_PLATFORM_WINDOWS
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        snprintf(timeBuf, sizeof(timeBuf), "crash_%04d%02d%02d_%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    }

    // Dropbox file request URLs accept multipart form uploads
    // URL format: https://www.dropbox.com/request/XXXXX
    // We POST to the file request endpoint with the file
    CURL* c = curl_easy_init();
    if (!c)
        return false;

    curl_mime* mime = curl_mime_init(c);

    // Upload the crash log as a text file
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_data(part, logContent.c_str(), CURL_ZERO_TERMINATED);
    std::string logFilename = std::string(timeBuf) + "_log.txt";
    curl_mime_filename(part, logFilename.c_str());
    curl_mime_type(part, "text/plain");

    // Upload the zip if available
    if (!zipPath.empty() && std::filesystem::exists(zipPath))
    {
        curl_mimepart* zipPart = curl_mime_addpart(mime);
        curl_mime_name(zipPart, "file");
        curl_mime_filedata(zipPart, zipPath.c_str());
        std::string zipFilename = std::string(timeBuf) + "_dump.zip";
        curl_mime_filename(zipPart, zipFilename.c_str());
    }

    std::string response;
    curl_easy_setopt(c, CURLOPT_URL, cfg.uploadURL.c_str());
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    ApplyTransferBounds(c, cfg);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

    CURLcode res = curl_easy_perform(c);
    bool httpOk = CheckHttpResponse(c, "Dropbox upload");
    curl_mime_free(mime);
    curl_easy_cleanup(c);

    bool success = (res == CURLE_OK && httpOk);
    if (success)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Dropbox upload succeeded");
        MarkUploadTime();
    }
    else
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "CrashReportUploader: Dropbox upload failed");
    }
    return success;
}

// ============================================================================
// FTP/FTPS upload
// ============================================================================

bool UploadCrashToFTP(const CrashConfig& cfg, const std::string& zipPath)
{
    if (cfg.uploadURL.empty())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashReportUploader: FTP URL not configured");
        return false;
    }

    if (zipPath.empty() || !std::filesystem::exists(zipPath))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashReportUploader: No dump file for FTP upload");
        return false;
    }

    if (IsCrashUploadRateLimited())
        return false;

    SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Uploading crash dump via FTP to %s",
                   cfg.uploadURL.c_str());

    std::string filename = std::filesystem::path(zipPath).filename().string();
    std::string ftpUrl = cfg.uploadURL;
    if (!ftpUrl.empty() && ftpUrl.back() != '/')
        ftpUrl += '/';
    ftpUrl += filename;

    // Read file into memory
    std::ifstream file(zipPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    auto fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> fileData(static_cast<size_t>(fileSize));
    file.read(fileData.data(), fileSize);
    file.close();

    CURL* c = curl_easy_init();
    if (!c)
        return false;

    curl_easy_setopt(c, CURLOPT_URL, ftpUrl.c_str());
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(fileSize));
    ApplyTransferBounds(c, cfg);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

    // Use a read callback to provide file data
    struct ReadState
    {
        const char* data;
        size_t remaining;
    };
    ReadState state{fileData.data(), static_cast<size_t>(fileSize)};

    curl_easy_setopt(
        c, CURLOPT_READFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t
        {
            auto* s = static_cast<ReadState*>(userdata);
            size_t maxBytes = size * nmemb;
            size_t toRead = (s->remaining < maxBytes) ? s->remaining : maxBytes;
            std::memcpy(ptr, s->data, toRead);
            s->data += toRead;
            s->remaining -= toRead;
            return toRead;
        });
    curl_easy_setopt(c, CURLOPT_READDATA, &state);

    // Create missing directories on the FTP server
    curl_easy_setopt(c, CURLOPT_FTP_CREATE_MISSING_DIRS, 1L);

    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);

    bool success = (res == CURLE_OK);
    if (success)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: FTP upload succeeded");
        MarkUploadTime();
    }
    else
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "CrashReportUploader: FTP upload failed (CURL error %d)",
                        static_cast<int>(res));
    }
    return success;
}

// ============================================================================
// SMTP email upload
// ============================================================================

// libcurl SMTP read callback — feeds RFC2822 email payload line by line
struct SmtpPayload
{
    std::string data;
    size_t offset = 0;
};

static size_t SmtpReadCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* payload = static_cast<SmtpPayload*>(userdata);
    size_t maxBytes = size * nmemb;
    if (payload->offset >= payload->data.size())
        return 0;
    size_t toRead = payload->data.size() - payload->offset;
    if (toRead > maxBytes)
        toRead = maxBytes;
    std::memcpy(ptr, payload->data.data() + payload->offset, toRead);
    payload->offset += toRead;
    return toRead;
}

bool UploadCrashToEmail(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath)
{
    if (cfg.emailTo.empty())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashReportUploader: Email recipient not configured");
        return false;
    }
    if (cfg.uploadURL.empty())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashReportUploader: SMTP server URL not configured");
        return false;
    }

    if (IsCrashUploadRateLimited())
        return false;

    SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Sending crash report via email to %s",
                   cfg.emailTo.c_str());

    // Build RFC2822 email payload
    std::string stackHash = ComputeStackHash(logContent);
    std::string subject = "SparkEngine Crash Report";
    if (!stackHash.empty())
        subject += " [" + stackHash + "]";

    // Include user description if provided
    std::string body = logContent;
    if (!cfg.userDescription.empty())
    {
        body = "=== User Description ===\n" + cfg.userDescription + "\n\n=== Crash Log ===\n" + body;
    }

    SmtpPayload payload;
    payload.data = "From: " + cfg.emailFrom +
                   "\r\n"
                   "To: " +
                   cfg.emailTo +
                   "\r\n"
                   "Subject: " +
                   subject +
                   "\r\n"
                   "Content-Type: text/plain; charset=UTF-8\r\n"
                   "\r\n" +
                   body + "\r\n";

    CURL* c = curl_easy_init();
    if (!c)
        return false;

    // SMTP URL (e.g., smtp://smtp.gmail.com:587 or smtps://smtp.gmail.com:465)
    curl_easy_setopt(c, CURLOPT_URL, cfg.uploadURL.c_str());

    // Authentication
    if (!cfg.smtpUser.empty())
    {
        curl_easy_setopt(c, CURLOPT_USERNAME, cfg.smtpUser.c_str());
        curl_easy_setopt(c, CURLOPT_PASSWORD, cfg.smtpPass.c_str());
    }

    // Use STARTTLS for smtp:// (smtps:// uses implicit TLS)
    if (cfg.uploadURL.starts_with("smtp://"))
        curl_easy_setopt(c, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));

    curl_easy_setopt(c, CURLOPT_MAIL_FROM, cfg.emailFrom.c_str());

    struct curl_slist* recipients = nullptr;
    recipients = curl_slist_append(recipients, cfg.emailTo.c_str());
    curl_easy_setopt(c, CURLOPT_MAIL_RCPT, recipients);

    curl_easy_setopt(c, CURLOPT_READFUNCTION, SmtpReadCallback);
    curl_easy_setopt(c, CURLOPT_READDATA, &payload);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    ApplyTransferBounds(c, cfg);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(c);

    bool success = (res == CURLE_OK);
    if (success)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Email sent successfully");
        MarkUploadTime();
    }
    else
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "CrashReportUploader: Email send failed (CURL error %d)",
                        static_cast<int>(res));
    }
    return success;
}

// ============================================================================
// Auto-detect backend and upload
// ============================================================================

bool UploadCrashReport(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath)
{
    if (!cfg.enableCrashReporting)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Crash reporting is disabled");
        return false;
    }

    // Priority 1: Proxy server (release builds)
    if (!cfg.proxyURL.empty())
        return UploadCrashToProxy(cfg, logContent, zipPath);

    // Priority 2: Direct GitHub API (dev builds with token)
    if (!cfg.githubRepo.empty() && !cfg.githubToken.empty())
        return UploadCrashToGitHub(cfg, logContent, zipPath);

    // Priority 3: Auto-detect from uploadURL
    const std::string& url = cfg.uploadURL;
    if (url.empty())
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core,
                       "CrashReportUploader: No upload URL configured, crash saved locally only");
        return false;
    }

    // Dropbox detection: dropbox.com in URL, or dbx:// scheme
    if (url.find("dropbox.com") != std::string::npos || url.starts_with("dbx://"))
        return UploadCrashToDropbox(cfg, logContent, zipPath);

    // FTP detection
    if (url.starts_with("ftp://") || url.starts_with("ftps://"))
        return UploadCrashToFTP(cfg, zipPath);

    // SMTP email detection
    if (url.starts_with("smtp://") || url.starts_with("smtps://"))
        return UploadCrashToEmail(cfg, logContent, zipPath);

    // Generic HTTP POST (default for any http(s):// URL)
    if (url.starts_with("http://") || url.starts_with("https://"))
    {
        if (IsCrashUploadRateLimited())
            return false;

        SPARK_LOG_INFO(Spark::LogCategory::Core, "CrashReportUploader: Uploading via generic HTTP POST to %s",
                       url.c_str());

        CURL* c = curl_easy_init();
        if (!c)
            return false;

        curl_mime* mime = curl_mime_init(c);

        // Log as text field
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "log");
        curl_mime_data(part, logContent.c_str(), CURL_ZERO_TERMINATED);

        // Stack hash
        std::string stackHash = ComputeStackHash(logContent);
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "stack_hash");
        curl_mime_data(part, stackHash.c_str(), CURL_ZERO_TERMINATED);

        // Dump file
        if (!zipPath.empty() && std::filesystem::exists(zipPath))
        {
            part = curl_mime_addpart(mime);
            curl_mime_name(part, "dump");
            curl_mime_filedata(part, zipPath.c_str());
        }

        std::string response;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
        ApplyTransferBounds(c, cfg);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

        CURLcode res = curl_easy_perform(c);
        bool httpOk = CheckHttpResponse(c, "generic HTTP upload");
        curl_mime_free(mime);
        curl_easy_cleanup(c);

        bool success = (res == CURLE_OK && httpOk);
        if (success)
            MarkUploadTime();
        return success;
    }

    SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashReportUploader: Unrecognized URL scheme: %s", url.c_str());
    return false;
}

#else // !SPARK_HAS_CURL

bool UploadCrashReport(const CrashConfig& /*cfg*/, const std::string& /*logContent*/, const std::string& /*zipPath*/)
{
    return false;
}

bool UploadCrashToGitHub(const CrashConfig& /*cfg*/, const std::string& /*logContent*/, const std::string& /*zipPath*/)
{
    return false;
}

bool UploadCrashToProxy(const CrashConfig& /*cfg*/, const std::string& /*logContent*/, const std::string& /*zipPath*/)
{
    return false;
}

bool UploadCrashToEmail(const CrashConfig& /*cfg*/, const std::string& /*logContent*/, const std::string& /*zipPath*/)
{
    return false;
}

bool UploadCrashToDropbox(const CrashConfig& /*cfg*/, const std::string& /*logContent*/, const std::string& /*zipPath*/)
{
    return false;
}

bool UploadCrashToFTP(const CrashConfig& /*cfg*/, const std::string& /*zipPath*/)
{
    return false;
}

bool UploadCrashFile(const CrashConfig& /*cfg*/, const std::string& /*url*/, const std::string& /*filePath*/,
                     const std::string& /*field*/)
{
    return false;
}

#endif // SPARK_HAS_CURL
