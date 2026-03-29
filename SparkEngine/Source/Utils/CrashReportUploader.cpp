/**
 * @file CrashReportUploader.cpp
 * @brief GitHub crash report upload implementation (extracted from CrashHandler)
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains the GitHub Issue creation and file upload logic that was previously
 * embedded in CrashHandler.cpp. All functions are no-ops when SPARK_HAS_CURL
 * is not defined.
 *
 * @see CrashReportUploader.h, CrashHandler.h
 */

#include "Utils/CrashReportUploader.h"
#include "Utils/CrashHandler.h"

#ifdef SPARK_HAS_CURL
#include <curl/curl.h>
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <ctime>

#ifdef SPARK_HAS_CURL

// libcurl write callback — appends response body to a std::string
static size_t GitHubWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* response = static_cast<std::string*>(userdata);
    size_t bytes = size * nmemb;
    response->append(ptr, bytes);
    return bytes;
}

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

// Create a GitHub Issue with the crash log as the issue body (markdown).
// Optionally uploads the zip file as a GitHub Release asset and links it.
// Returns true on success.
bool UploadCrashToGitHub(const CrashConfig& cfg, const std::string& logContent, const std::string& zipPath)
{
    if (cfg.githubRepo.empty() || cfg.githubToken.empty())
        return false;

    // ---- Build issue body ----
    std::ostringstream body;
    body << "## Automated Crash Report\\n\\n";
    body << "This issue was automatically created by the SparkEngine crash handler.\\n\\n";
    body << "### Crash Log\\n\\n";
    body << "```\\n" << JsonEscape(logContent) << "\\n```\\n";

    // ---- Build issue title ----
    // Extract a short summary from the log (first meaningful line after the header)
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
        // Create a tag-less release (draft) to host the crash dump asset
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
            struct curl_slist* headers = nullptr;
            std::string authHeader = "Authorization: Bearer " + cfg.githubToken;
            headers = curl_slist_append(headers, authHeader.c_str());
            headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
            headers = curl_slist_append(headers, "User-Agent: SparkEngine-CrashHandler/1.0");

            curl_easy_setopt(c, CURLOPT_URL, releaseUrl.c_str());
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, releaseJson.c_str());
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &releaseResponse);
            curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, static_cast<long>(cfg.connectTimeoutSeconds));
            curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

            CURLcode res = curl_easy_perform(c);
            curl_slist_free_all(headers);
            curl_easy_cleanup(c);

            // Parse upload_url from response (simple string search — avoids JSON library dependency)
            if (res == CURLE_OK)
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

                    // Upload the zip file as an asset
                    std::string filename = std::filesystem::path(zipPath).filename().string();
                    uploadUrl += "?name=" + filename;

                    // Read zip into memory
                    std::ifstream zipFile(zipPath, std::ios::binary | std::ios::ate);
                    if (zipFile.is_open())
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
                            struct curl_slist* h2 = nullptr;
                            h2 = curl_slist_append(h2, authHeader.c_str());
                            h2 = curl_slist_append(h2, "Accept: application/vnd.github+json");
                            h2 = curl_slist_append(h2, "Content-Type: application/zip");
                            h2 = curl_slist_append(h2, "X-GitHub-Api-Version: 2022-11-28");
                            h2 = curl_slist_append(h2, "User-Agent: SparkEngine-CrashHandler/1.0");

                            curl_easy_setopt(c2, CURLOPT_URL, uploadUrl.c_str());
                            curl_easy_setopt(c2, CURLOPT_HTTPHEADER, h2);
                            curl_easy_setopt(c2, CURLOPT_POSTFIELDS, fileData.data());
                            curl_easy_setopt(c2, CURLOPT_POSTFIELDSIZE, static_cast<long>(fileSize));
                            curl_easy_setopt(c2, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
                            curl_easy_setopt(c2, CURLOPT_WRITEDATA, &assetResponse);
                            curl_easy_setopt(c2, CURLOPT_CONNECTTIMEOUT, static_cast<long>(cfg.connectTimeoutSeconds));
                            curl_easy_setopt(c2, CURLOPT_NOPROGRESS, 1L);

                            CURLcode res2 = curl_easy_perform(c2);
                            curl_slist_free_all(h2);
                            curl_easy_cleanup(c2);

                            if (res2 == CURLE_OK)
                            {
                                // Extract browser_download_url from asset response
                                std::string dlNeedle = "\"browser_download_url\":\"";
                                size_t dlPos = assetResponse.find(dlNeedle);
                                if (dlPos != std::string::npos)
                                {
                                    dlPos += dlNeedle.size();
                                    size_t dlEnd = assetResponse.find('"', dlPos);
                                    assetLink = assetResponse.substr(dlPos, dlEnd - dlPos);
                                }
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
            // Trim whitespace
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

    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + cfg.githubToken;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    headers = curl_slist_append(headers, "User-Agent: SparkEngine-CrashHandler/1.0");

    curl_easy_setopt(c, CURLOPT_URL, issueUrl.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, issueJson.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, GitHubWriteCallback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &issueResponse);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, static_cast<long>(cfg.connectTimeoutSeconds));
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    return (res == CURLE_OK && issueResponse.contains("\"id\""));
}

bool UploadCrashFile(const CrashConfig& cfg, const std::string& url, const std::string& filePath,
                     const std::string& field)
{
    CURL* c = curl_easy_init();
    if (!c)
        return false;
    curl_mime* mime = curl_mime_init(c);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, field.c_str());
    curl_mime_filedata(part, filePath.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, cfg.connectTimeoutSeconds);
    CURLcode res = curl_easy_perform(c);
    curl_mime_free(mime);
    curl_easy_cleanup(c);
    return (res == CURLE_OK);
}

#else // !SPARK_HAS_CURL

bool UploadCrashToGitHub(const CrashConfig& /*cfg*/, const std::string& /*logContent*/, const std::string& /*zipPath*/)
{
    return false;
}

bool UploadCrashFile(const CrashConfig& /*cfg*/, const std::string& /*url*/, const std::string& /*filePath*/,
                     const std::string& /*field*/)
{
    return false;
}

#endif // SPARK_HAS_CURL
