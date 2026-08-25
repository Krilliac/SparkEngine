/**
 * @file ProjectManager.cpp
 * @brief Implementation of the project management system
 * @author Spark Engine Team
 * @date 2025
 */

#include "ProjectManager.h"
#include "Utils/ContainerUtils.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Validate.h"
#include "Engine/ECS/Components.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <random>
#include <set>
#include <array>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace SparkEditor
{

    // ------------------------------------------------------------------
    // EngineVersion
    // ------------------------------------------------------------------
    std::string EngineVersion::ToString() const
    {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    static EngineVersion GetCurrentEngineVersion()
    {
        return {EDITOR_VERSION_MAJOR, EDITOR_VERSION_MINOR, EDITOR_VERSION_PATCH};
    }

    static uint64_t GetCurrentTimestamp()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    // ------------------------------------------------------------------
    // Simple JSON helpers (write-only, no dependency)
    // ------------------------------------------------------------------
    static std::string EscapeJsonString(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
            case '\"':
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
                    constexpr char hex[] = "0123456789ABCDEF";
                    const unsigned char value = static_cast<unsigned char>(c);
                    out += "\\u00";
                    out += hex[value >> 4];
                    out += hex[value & 0x0F];
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

    static int JsonHexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    static bool ParseJsonHex4(const std::string& json, size_t offset, uint32_t& value)
    {
        if (offset + 4 > json.size())
            return false;
        value = 0;
        for (size_t index = 0; index < 4; ++index)
        {
            const int digit = JsonHexValue(json[offset + index]);
            if (digit < 0)
                return false;
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        return true;
    }

    static bool AppendUtf8(uint32_t codePoint, std::string& output)
    {
        if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return false;
        if (codePoint <= 0x7F)
            output.push_back(static_cast<char>(codePoint));
        else if (codePoint <= 0x7FF)
        {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint <= 0xFFFF)
        {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        return true;
    }

    // `index` points at the character immediately after a JSON backslash and
    // advances across any consumed unicode digits/surrogate pair.
    static bool DecodeJsonEscape(const std::string& json, size_t& index, std::string& output)
    {
        switch (json[index])
        {
        case '"':
            output += '"';
            return true;
        case '\\':
            output += '\\';
            return true;
        case '/':
            output += '/';
            return true;
        case 'b':
            output += '\b';
            return true;
        case 'f':
            output += '\f';
            return true;
        case 'n':
            output += '\n';
            return true;
        case 'r':
            output += '\r';
            return true;
        case 't':
            output += '\t';
            return true;
        case 'u':
        {
            uint32_t first = 0;
            if (!ParseJsonHex4(json, index + 1, first))
                return false;
            index += 4;
            if (first >= 0xD800 && first <= 0xDBFF)
            {
                if (index + 6 >= json.size() || json[index + 1] != '\\' || json[index + 2] != 'u')
                    return false;
                uint32_t second = 0;
                if (!ParseJsonHex4(json, index + 3, second) || second < 0xDC00 || second > 0xDFFF)
                    return false;
                first = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                index += 6;
            }
            else if (first >= 0xDC00 && first <= 0xDFFF)
            {
                return false;
            }
            return AppendUtf8(first, output);
        }
        default:
            return false;
        }
    }

    static std::string ExtractJsonString(const std::string& json, const std::string& key)
    {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return "";
        pos = json.find(':', pos);
        if (pos == std::string::npos)
            return "";
        pos = json.find('\"', pos + 1);
        if (pos == std::string::npos)
            return "";
        std::string result;
        for (size_t i = pos + 1; i < json.size(); ++i)
        {
            const char c = json[i];
            if (c == '\"')
                return result;
            if (c != '\\')
            {
                result += c;
                continue;
            }
            if (++i >= json.size())
                return "";
            if (!DecodeJsonEscape(json, i, result))
                return "";
        }
        return "";
    }

    static uint64_t ExtractJsonUint64(const std::string& json, const std::string& key)
    {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return 0;
        pos = json.find(':', pos);
        if (pos == std::string::npos)
            return 0;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
            pos++;
        std::string numStr;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
        {
            numStr += json[pos++];
        }
        if (numStr.empty())
            return 0;
        return std::stoull(numStr);
    }

    static std::vector<std::string> ExtractJsonStringArray(const std::string& json, const std::string& key)
    {
        std::vector<std::string> result;
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return result;
        pos = json.find('[', pos);
        if (pos == std::string::npos)
            return result;
        size_t end = json.find(']', pos);
        if (end == std::string::npos)
            return result;

        size_t i = pos + 1;
        while (i < end)
        {
            size_t qStart = json.find('\"', i);
            if (qStart == std::string::npos || qStart >= end)
                break;
            std::string value;
            bool closed = false;
            for (i = qStart + 1; i < end; ++i)
            {
                char c = json[i];
                if (c == '\"')
                {
                    ++i;
                    closed = true;
                    break;
                }
                if (c == '\\' && i + 1 < end)
                {
                    ++i;
                    if (!DecodeJsonEscape(json, i, value) || i >= end)
                        return {};
                    continue;
                }
                else if (c == '\\')
                {
                    return {};
                }
                value += c;
            }
            if (!closed)
                break;
            result.push_back(std::move(value));
        }
        return result;
    }

    // ------------------------------------------------------------------
    // ProjectManager
    // ------------------------------------------------------------------
    std::mutex ProjectManager::s_activeProjectMutex;
    std::string ProjectManager::s_activeProjectPath;

    namespace
    {
        constexpr std::array<std::string_view, 2> kEmptyFeatures = {"minimal", "scene-editing"};
        constexpr std::array<std::string_view, 6> kFirstPersonFeatures = {"movement", "mouselook", "weapons",
                                                                          "damage",   "hud",       "restart"};
        constexpr std::array<std::string_view, 5> kThirdPersonFeatures = {"movement", "orbit-camera", "jump", "pickup",
                                                                          "goal"};
        constexpr std::array<std::string_view, 6> kTopDownFeatures = {"movement", "pan-zoom-camera", "collision",
                                                                      "enemy",    "pickup",          "restart"};
        constexpr std::array<std::string_view, 4> kBlank3DFeatures = {"primitive", "ground", "lighting", "fly-camera"};
        constexpr std::array<std::string_view, 7> kMmoFeatures = {
            "local-client-server", "character", "faction", "capture-point", "bot", "chat", "respawn"};
        constexpr std::array<std::string_view, 8> kPlatformerFeatures = {
            "movement", "jump", "double-jump", "collectibles", "hazard", "checkpoint", "finish", "restart"};
        constexpr std::array<std::string_view, 7> kRpgFeatures = {"movement", "dialogue", "quest",    "inventory",
                                                                  "combat",   "reward",   "save-load"};

        constexpr std::array<ProjectTemplateDescriptor, 8> kProjectTemplates = {{
            {ProjectTemplate::Blank3D, "blank-3d", "Blank 3D",
             "A ready-to-edit 3D scene with a visible primitive, ground, lighting, and fly camera.", "[3D]", "Blank3D",
             "Scenes/Default.sparkscene", "General", kBlank3DFeatures, true},
            {ProjectTemplate::FirstPerson, "first-person", "First Person",
             "A compact first-person game with movement, weapons, a damageable target, HUD, and restart loop.", "[FP]",
             "FPSStarter", "Scenes/Arena.sparkscene", "FPS", kFirstPersonFeatures, false},
            {ProjectTemplate::ThirdPerson, "third-person", "Third Person",
             "A third-person adventure slice with orbit camera, jumping, a pickup, and a goal.", "[TP]",
             "ThirdPersonStarter", "Scenes/Adventure.sparkscene", "Adventure", kThirdPersonFeatures, false},
            {ProjectTemplate::TopDown, "top-down", "Top Down",
             "A top-down action slice with pan/zoom camera, collision, an enemy, a pickup, and restart.", "[TD]",
             "TopDownStarter", "Scenes/Skirmish.sparkscene", "Action", kTopDownFeatures, false},
            {ProjectTemplate::Platformer, "platformer", "Platformer",
             "A complete platformer level with double jump, collectibles, hazards, checkpoint, finish, and restart.",
             "[PL]", "PlatformerKit", "Scenes/Level01.sparkscene", "Platformer", kPlatformerFeatures, false},
            {ProjectTemplate::RPG, "rpg", "RPG",
             "A village RPG slice with dialogue, quest, inventory, combat, reward, and save/load.", "[RPG]",
             "RPGStarter", "Scenes/Village.sparkscene", "RPG", kRpgFeatures, false},
            {ProjectTemplate::MMO, "mmo", "MMO",
             "A bounded local client/server sample with character setup, faction objective, bot, chat, and respawn.",
             "[MMO]", "MMOStarter", "Scenes/Frontier.sparkscene", "MMO", kMmoFeatures, false},
            {ProjectTemplate::Empty, "empty", "Empty",
             "A truthful empty project with an editable world and no bundled art or gameplay assumptions.", "[ ]",
             "EmptyProject", "Scenes/Default.sparkscene", "General", kEmptyFeatures, true},
        }};

        static_assert(kProjectTemplates.size() == 8);

        fs::path GetProcessExecutablePath()
        {
#ifdef _WIN32
            std::wstring buffer(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length >= buffer.size())
                return {};
            buffer.resize(length);
            return fs::path(buffer);
#elif defined(__APPLE__)
            uint32_t size = 0;
            if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0)
                return {};
            std::string buffer(size, '\0');
            if (_NSGetExecutablePath(buffer.data(), &size) != 0)
                return {};
            return fs::path(buffer.c_str());
#else
            std::array<char, 4096> buffer{};
            const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
            if (length <= 0 || static_cast<size_t>(length) == buffer.size() - 1)
                return {};
            return fs::path(std::string(buffer.data(), static_cast<size_t>(length)));
#endif
        }

        fs::path PathFromUtf8(std::string_view value)
        {
#ifdef _WIN32
            if (value.empty())
                return {};
            if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                throw std::runtime_error("UTF-8 path is too long");
            const int sourceLength = static_cast<int>(value.size());
            const int wideLength =
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength, nullptr, 0);
            if (wideLength <= 0)
                throw std::runtime_error("Path is not valid UTF-8");
            std::wstring wide(static_cast<size_t>(wideLength), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength, wide.data(),
                                    wideLength) != wideLength)
                throw std::runtime_error("Could not convert UTF-8 path");
            return fs::path(wide);
#else
            return fs::path(value);
#endif
        }

        std::string PathToUtf8(const fs::path& value)
        {
#ifdef _WIN32
            const std::wstring& wide = value.native();
            if (wide.empty())
                return {};
            if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                throw std::runtime_error("Native path is too long");
            const int sourceLength = static_cast<int>(wide.size());
            const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), sourceLength,
                                                       nullptr, 0, nullptr, nullptr);
            if (utf8Length <= 0)
                throw std::runtime_error("Could not measure UTF-8 path");
            std::string utf8(static_cast<size_t>(utf8Length), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), sourceLength, utf8.data(), utf8Length,
                                    nullptr, nullptr) != utf8Length)
                throw std::runtime_error("Could not convert native path to UTF-8");
            return utf8;
#else
            return value.string();
#endif
        }

        void AddTemplateRootCandidates(const fs::path& base, std::vector<fs::path>& candidates)
        {
            if (base.empty())
                return;
            candidates.push_back(base);
            candidates.push_back(base / "Templates");
            candidates.push_back(base / "share" / "SparkEngine" / "templates");
        }

        std::string ResolveTemplateRoot(const std::string& explicitRoot)
        {
            std::vector<fs::path> candidates;
            if (!explicitRoot.empty())
                AddTemplateRootCandidates(PathFromUtf8(explicitRoot), candidates);

            const bool hasExplicitRoot = !explicitRoot.empty();

            if (!hasExplicitRoot)
            {
                if (const char* configuredRoot = std::getenv("SPARK_ENGINE_ROOT"))
                    AddTemplateRootCandidates(configuredRoot, candidates);
            }

            auto addAncestors = [&candidates](fs::path current)
            {
                for (int depth = 0; !current.empty() && depth < 8; ++depth)
                {
                    AddTemplateRootCandidates(current, candidates);
                    const fs::path parent = current.parent_path();
                    if (parent == current)
                        break;
                    current = parent;
                }
            };

            if (!hasExplicitRoot)
                addAncestors(GetProcessExecutablePath().parent_path());

            for (const fs::path& candidate : candidates)
            {
                std::error_code ec;
                const fs::path canonical = fs::weakly_canonical(candidate, ec);
                if (ec || !fs::is_directory(canonical, ec) || ec)
                    continue;
                bool hasPackage = false;
                for (fs::directory_iterator it(canonical, fs::directory_options::skip_permission_denied, ec), end;
                     !ec && it != end; it.increment(ec))
                {
                    if (it->is_directory(ec) && !ec && fs::is_regular_file(it->path() / "template.json", ec) && !ec)
                    {
                        hasPackage = true;
                        break;
                    }
                }
                if (hasPackage)
                    return PathToUtf8(canonical);
            }
            return {};
        }

        bool IsValidUtf8(std::string_view value)
        {
            for (size_t i = 0; i < value.size();)
            {
                const auto byte = static_cast<unsigned char>(value[i]);
                size_t trailing = 0;
                if (byte <= 0x7F)
                {
                    ++i;
                    continue;
                }
                if (byte >= 0xC2 && byte <= 0xDF)
                    trailing = 1;
                else if (byte >= 0xE0 && byte <= 0xEF)
                    trailing = 2;
                else if (byte >= 0xF0 && byte <= 0xF4)
                    trailing = 3;
                else
                    return false;
                if (i + trailing >= value.size())
                    return false;
                const auto second = static_cast<unsigned char>(value[i + 1]);
                if ((second & 0xC0) != 0x80 || (byte == 0xE0 && second < 0xA0) || (byte == 0xED && second >= 0xA0) ||
                    (byte == 0xF0 && second < 0x90) || (byte == 0xF4 && second >= 0x90))
                    return false;
                for (size_t offset = 2; offset <= trailing; ++offset)
                {
                    if ((static_cast<unsigned char>(value[i + offset]) & 0xC0) != 0x80)
                        return false;
                }
                i += trailing + 1;
            }
            return true;
        }

        std::string NormalizeProjectPath(const std::string& value)
        {
            if (value.empty())
                return {};
            std::error_code ec;
            fs::path normalized = fs::absolute(PathFromUtf8(value), ec);
            if (ec)
            {
                ec.clear();
                normalized = PathFromUtf8(value);
            }
            const fs::path canonical = fs::weakly_canonical(normalized, ec);
            if (!ec)
                normalized = canonical;
            return PathToUtf8(normalized.lexically_normal());
        }

        bool ProjectPathsEqual(const std::string& left, const std::string& right)
        {
            std::string normalizedLeft = NormalizeProjectPath(left);
            std::string normalizedRight = NormalizeProjectPath(right);
#ifdef _WIN32
            std::transform(normalizedLeft.begin(), normalizedLeft.end(), normalizedLeft.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(normalizedRight.begin(), normalizedRight.end(), normalizedRight.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
            return normalizedLeft == normalizedRight;
        }

        bool IsPathInsideRoot(const fs::path& root, const fs::path& candidate)
        {
            std::string rootText = PathToUtf8(root.lexically_normal());
            std::string candidateText = PathToUtf8(candidate.lexically_normal());
            std::replace(rootText.begin(), rootText.end(), '\\', '/');
            std::replace(candidateText.begin(), candidateText.end(), '\\', '/');
#ifdef _WIN32
            std::transform(rootText.begin(), rootText.end(), rootText.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(candidateText.begin(), candidateText.end(), candidateText.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
            while (rootText.size() > 1 && rootText.back() == '/')
                rootText.pop_back();
            return candidateText.size() > rootText.size() && candidateText.starts_with(rootText) &&
                   candidateText[rootText.size()] == '/';
        }

        constexpr uint64_t kMaximumSceneDocumentBytes = 64ull * 1024ull * 1024ull;

        bool ReadContainedFileFromHandle(const fs::path& projectRoot, const fs::path& candidate,
                                         std::string& resolvedPath, std::string& contents)
        {
            resolvedPath.clear();
            contents.clear();
#ifdef _WIN32
            const HANDLE rawHandle =
                ::CreateFileW(candidate.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (rawHandle == INVALID_HANDLE_VALUE)
                return false;
            struct HandleCloser
            {
                HANDLE handle;
                ~HandleCloser() { ::CloseHandle(handle); }
            } closer{rawHandle};

            BY_HANDLE_FILE_INFORMATION information{};
            if (!::GetFileInformationByHandle(rawHandle, &information) ||
                (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
                ::GetFileType(rawHandle) != FILE_TYPE_DISK)
            {
                return false;
            }

            const DWORD pathLength =
                ::GetFinalPathNameByHandleW(rawHandle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (pathLength == 0)
                return false;
            std::vector<wchar_t> finalPathBuffer(static_cast<size_t>(pathLength) + 1u, L'\0');
            const DWORD copied = ::GetFinalPathNameByHandleW(rawHandle, finalPathBuffer.data(),
                                                             static_cast<DWORD>(finalPathBuffer.size()),
                                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (copied == 0 || copied >= finalPathBuffer.size())
                return false;

            std::wstring finalPathText(finalPathBuffer.data(), copied);
            if (finalPathText.starts_with(L"\\\\?\\UNC\\"))
                finalPathText = L"\\\\" + finalPathText.substr(8);
            else if (finalPathText.starts_with(L"\\\\?\\"))
                finalPathText.erase(0, 4);
            const fs::path finalPath = fs::path(finalPathText).lexically_normal();
            if (!IsPathInsideRoot(projectRoot, finalPath))
                return false;

            LARGE_INTEGER fileSize{};
            if (!::GetFileSizeEx(rawHandle, &fileSize) || fileSize.QuadPart < 0 ||
                static_cast<uint64_t>(fileSize.QuadPart) > kMaximumSceneDocumentBytes)
            {
                return false;
            }
            contents.resize(static_cast<size_t>(fileSize.QuadPart));
            size_t offset = 0;
            while (offset < contents.size())
            {
                const DWORD requested =
                    static_cast<DWORD>(std::min<size_t>(contents.size() - offset, static_cast<size_t>(1024u * 1024u)));
                DWORD bytesRead = 0;
                if (!::ReadFile(rawHandle, contents.data() + offset, requested, &bytesRead, nullptr) || bytesRead == 0)
                    return false;
                offset += bytesRead;
            }
            resolvedPath = PathToUtf8(finalPath);
            return true;
#else
            int flags = O_RDONLY;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
            const int descriptor = ::open(candidate.c_str(), flags);
            if (descriptor < 0)
                return false;
            struct DescriptorCloser
            {
                int descriptor;
                ~DescriptorCloser() { ::close(descriptor); }
            } closer{descriptor};

            struct stat information{};
            if (::fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) || information.st_size < 0 ||
                static_cast<uint64_t>(information.st_size) > kMaximumSceneDocumentBytes)
            {
                return false;
            }

            fs::path finalPath;
#ifdef __APPLE__
            std::array<char, PATH_MAX> finalPathBuffer{};
            if (::fcntl(descriptor, F_GETPATH, finalPathBuffer.data()) == -1)
                return false;
            finalPath = fs::path(finalPathBuffer.data()).lexically_normal();
#else
            const std::string descriptorPath = "/proc/self/fd/" + std::to_string(descriptor);
            std::array<char, PATH_MAX> finalPathBuffer{};
            const ssize_t copied =
                ::readlink(descriptorPath.c_str(), finalPathBuffer.data(), finalPathBuffer.size() - 1);
            if (copied <= 0 || static_cast<size_t>(copied) >= finalPathBuffer.size())
                return false;
            finalPathBuffer[static_cast<size_t>(copied)] = '\0';
            finalPath = fs::path(finalPathBuffer.data()).lexically_normal();
#endif
            if (!IsPathInsideRoot(projectRoot, finalPath))
                return false;

            contents.resize(static_cast<size_t>(information.st_size));
            size_t offset = 0;
            while (offset < contents.size())
            {
                const ssize_t bytesRead = ::read(descriptor, contents.data() + offset, contents.size() - offset);
                if (bytesRead <= 0)
                    return false;
                offset += static_cast<size_t>(bytesRead);
            }
            resolvedPath = PathToUtf8(finalPath);
            return true;
#endif
        }

        std::string MakeCodeIdentifier(const std::string& value)
        {
            std::string result;
            result.reserve(value.size() + 8);
            for (unsigned char c : value)
            {
                const bool asciiAlphaNumeric =
                    (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                if (asciiAlphaNumeric || (c == '_' && (result.empty() || result.back() != '_')))
                    result.push_back(static_cast<char>(c));
                else if (result.empty() || result.back() != '_')
                    result.push_back('_');
            }
            if (result.empty())
                result = "SparkGame";
            if (!((result.front() >= 'a' && result.front() <= 'z') || (result.front() >= 'A' && result.front() <= 'Z')))
                result.insert(0, "SparkGame");
            static const std::set<std::string> cppKeywords = {
                "alignas",       "alignof",     "and",        "and_eq",   "asm",       "auto",
                "bitand",        "bitor",       "bool",       "break",    "case",      "catch",
                "char",          "class",       "compl",      "concept",  "const",     "consteval",
                "constexpr",     "constinit",   "const_cast", "continue", "co_await",  "co_return",
                "co_yield",      "decltype",    "default",    "delete",   "do",        "double",
                "dynamic_cast",  "else",        "enum",       "explicit", "export",    "extern",
                "false",         "float",       "for",        "friend",   "goto",      "if",
                "inline",        "int",         "long",       "mutable",  "namespace", "new",
                "noexcept",      "not",         "not_eq",     "nullptr",  "operator",  "or",
                "or_eq",         "private",     "protected",  "public",   "register",  "reinterpret_cast",
                "requires",      "return",      "short",      "signed",   "sizeof",    "static",
                "static_assert", "static_cast", "struct",     "switch",   "template",  "this",
                "thread_local",  "throw",       "true",       "try",      "typedef",   "typeid",
                "typename",      "union",       "unsigned",   "using",    "virtual",   "void",
                "volatile",      "wchar_t",     "while",      "xor",      "xor_eq"};
            if (cppKeywords.contains(result))
                result.insert(0, "SparkGame_");
            if (result.size() > 63)
            {
                uint32_t hash = 2166136261u;
                for (unsigned char c : value)
                {
                    hash ^= c;
                    hash *= 16777619u;
                }
                constexpr char hex[] = "0123456789abcdef";
                std::string suffix(9, '_');
                for (int nibble = 0; nibble < 8; ++nibble)
                    suffix[8 - nibble] = hex[(hash >> (nibble * 4)) & 0x0F];
                result.resize(63 - suffix.size());
                while (!result.empty() && result.back() == '_')
                    result.pop_back();
                result += suffix;
            }
            return result;
        }

        bool IsPortableProjectName(const std::string& value)
        {
            if (value.empty() || value.size() > 120 || !IsValidUtf8(value) || value == "." || value == ".." ||
                value.front() == ' ' || value.back() == ' ' || value.back() == '.')
                return false;
            static constexpr std::string_view invalid = "\\/:*?\"<>|";
            for (unsigned char c : value)
            {
                if (c < 0x20 || invalid.find(static_cast<char>(c)) != std::string_view::npos)
                    return false;
            }

            std::string stem = value;
            if (const size_t dot = stem.find('.'); dot != std::string::npos)
                stem.resize(dot);
            std::transform(stem.begin(), stem.end(), stem.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            for (const auto& [encoded, ascii] : std::array<std::pair<std::string_view, char>, 3>{
                     {{"\xC2\xB9", '1'}, {"\xC2\xB2", '2'}, {"\xC2\xB3", '3'}}})
            {
                if (const size_t pos = stem.find(encoded); pos != std::string::npos)
                    stem.replace(pos, encoded.size(), 1, ascii);
            }
            static const std::set<std::string> reserved = {"CON",  "PRN",    "AUX",    "NUL",    "COM1", "COM2", "COM3",
                                                           "COM4", "COM5",   "COM6",   "COM7",   "COM8", "COM9", "LPT1",
                                                           "LPT2", "LPT3",   "LPT4",   "LPT5",   "LPT6", "LPT7", "LPT8",
                                                           "LPT9", "CLOCK$", "CONIN$", "CONOUT$"};
            return !reserved.contains(stem);
        }

        fs::path CreateOwnedStagingDirectory(const fs::path& destination)
        {
            std::error_code ec;
            fs::create_directories(destination.parent_path(), ec);
            if (ec)
                return {};

            std::random_device entropy;
            const uint64_t now =
                static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
            for (uint64_t attempt = 0; attempt < 64; ++attempt)
            {
                const uint64_t nonce = now ^ (static_cast<uint64_t>(entropy()) << 32) ^ entropy() ^ attempt;
                fs::path candidate = destination;
#ifdef _WIN32
                candidate += L".spark-staging-" + std::to_wstring(nonce);
#else
                candidate += ".spark-staging-" + std::to_string(nonce);
#endif
                ec.clear();
                if (fs::create_directory(candidate, ec))
                    return candidate;
                if (ec && ec != std::errc::file_exists)
                    return {};
            }
            return {};
        }

        bool IsLinkOrReparsePoint(const fs::path& path)
        {
            std::error_code ec;
            const fs::file_status status = fs::symlink_status(path, ec);
            if (ec || fs::is_symlink(status))
                return true;
#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
            return false;
#endif
        }

        bool TemplateTreeContainsLinks(const fs::path& root)
        {
            std::error_code ec;
            if (IsLinkOrReparsePoint(root))
                return true;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                if (IsLinkOrReparsePoint(it->path()))
                    return true;
            }
            return static_cast<bool>(ec);
        }

        std::string EscapeCppString(const std::string& value)
        {
            std::string result;
            result.reserve(value.size() + 8);
            for (char c : value)
            {
                switch (c)
                {
                case '\\':
                    result += "\\\\";
                    break;
                case '\"':
                    result += "\\\"";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    result += c;
                    break;
                }
            }
            return result;
        }

        bool WriteNewFile(const fs::path& path, const std::string& contents)
        {
            if (fs::exists(path))
                return true;
            std::ofstream file(path, std::ios::binary);
            if (!file.is_open())
                return false;
            file << contents;
            return file.good();
        }
    } // namespace

    ProjectManager::ProjectManager() = default;
    ProjectManager::~ProjectManager() = default;

    bool ProjectManager::Initialize()
    {
        std::cout << "ProjectManager::Initialize()\n";

        // Ensure editor data directory exists
        std::string editorDataDir = GetEditorDataDirectory();
        try
        {
            fs::create_directories(editorDataDir);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Warning: Could not create editor data directory: " << e.what() << "\n";
        }

        LoadRecentProjectsList();
        m_isInitialized = true;
        return true;
    }

    void ProjectManager::Shutdown()
    {
        std::cout << "ProjectManager::Shutdown()\n";
        if (m_hasOpenProject)
        {
            CloseProject();
        }
        SaveRecentProjectsList();
        m_isInitialized = false;
    }

    // ------------------------------------------------------------------
    // Project lifecycle
    // ------------------------------------------------------------------
    bool ProjectManager::CreateProject(const std::string& projectName, const std::string& parentDirectory,
                                       ProjectTemplate templateType, const std::string& description)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, IsPortableProjectName(projectName), false);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !parentDirectory.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Creating project '%s' in '%s'", projectName.c_str(),
                       parentDirectory.c_str());
        // Prefer the matching physical package when a coherent source/install
        // template root is available. A missing package falls back to the
        // serializer-backed generator below, so fresh source builds and lean
        // runtime installs still create usable projects.
        const auto* descriptor = FindProjectTemplateDescriptor(templateType);
        std::string templateRoot;
        try
        {
            templateRoot = ResolveTemplateRoot(m_engineRoot);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Could not resolve the SparkEngine template root: " << e.what() << "\n";
            return false;
        }
        std::error_code packageEc;
        const bool hasPhysicalPackage =
            descriptor && !templateRoot.empty() &&
            fs::is_directory(PathFromUtf8(templateRoot) / descriptor->packageDirectory, packageEc) && !packageEc;
        if (hasPhysicalPackage)
        {
            bool created = false;
            try
            {
                created = CreateProjectFromTemplateInternal(
                    projectName, PathToUtf8(PathFromUtf8(parentDirectory) / PathFromUtf8(projectName)),
                    std::string(descriptor->packageDirectory), description, templateRoot);
            }
            catch (const std::exception& e)
            {
                std::cerr << "Error preparing physical template project creation: " << e.what() << "\n";
                return false;
            }
            if (created)
                return true;
            // A package that exists but fails to materialize is not silently
            // replaced with different content; retain the specific failure.
            return false;
        }

        std::cout << "Creating project '" << projectName << "' in " << parentDirectory << "\n";

        const ProjectInfo previousProject = m_currentProject;
        const std::string previousProjectFilePath = m_currentProjectFilePath;
        const bool previouslyOpen = m_hasOpenProject;
        std::error_code destinationEc;
        fs::path destination;
        try
        {
            destination = fs::absolute(PathFromUtf8(parentDirectory) / PathFromUtf8(projectName), destinationEc)
                              .lexically_normal();
        }
        catch (const std::exception& e)
        {
            std::cerr << "Project destination path is invalid: " << e.what() << "\n";
            return false;
        }
        if (destinationEc || destination.filename().empty())
        {
            std::cerr << "Project destination path is invalid.\n";
            return false;
        }
        fs::path staging;
        bool ownsStaging = false;
        bool committed = false;

        auto fail = [&](const std::string& message)
        {
            if (!committed)
            {
                if (ownsStaging)
                {
                    std::error_code cleanupEc;
                    fs::remove_all(staging, cleanupEc);
                    if (cleanupEc)
                        std::cerr << "Could not remove failed project staging directory '" << PathToUtf8(staging)
                                  << "': " << cleanupEc.message() << "\n";
                }
                m_currentProject = previousProject;
                m_currentProjectFilePath = previousProjectFilePath;
                m_hasOpenProject = previouslyOpen;
            }
            if (!message.empty())
                std::cerr << message << "\n";
            return false;
        };

        try
        {
            if (fs::exists(destination))
                return fail("Project destination already exists: " + PathToUtf8(destination));

            staging = CreateOwnedStagingDirectory(destination);
            if (staging.empty())
                return fail("Could not create an isolated project staging directory.");
            ownsStaging = true;

            if (!CreateProjectStructure(PathToUtf8(staging), projectName, templateType))
                return fail("Could not create the generated project structure.");

            // Fill project info
            m_currentProject = ProjectInfo{};
            m_currentProject.name = projectName;
            m_currentProject.path = NormalizeProjectPath(PathToUtf8(staging));
            m_currentProjectFilePath = NormalizeProjectPath(
                PathToUtf8(PathFromUtf8(m_currentProject.path) / PathFromUtf8(projectName + ".sparkproject")));
            m_currentProject.version = "1.0.0";
            m_currentProject.description = description.empty() ? "Spark Engine Project" : description;
            m_currentProject.engineVersion = GetCurrentEngineVersion().ToString();
            m_currentProject.createdTime = GetCurrentTimestamp();
            m_currentProject.lastModified = m_currentProject.createdTime;
            m_currentProject.templateType = templateType;
            m_currentProject.hasTemplateIdentity = true;
            m_currentProject.modules.push_back(MakeCodeIdentifier(projectName));

            // Add the descriptor-selected default scene. The fallback
            // generator and physical package path therefore expose identical
            // project metadata.
            const std::string defaultSceneRelative =
                descriptor ? std::string(descriptor->defaultScene) : "Scenes/Default.sparkscene";
            m_currentProject.scenes.push_back(defaultSceneRelative);
            m_currentProject.defaultScene = defaultSceneRelative;
            m_currentProject.lastOpenedScene = defaultSceneRelative;

            if (!SaveProjectFile())
                return fail("Could not write the generated project document.");
            if (!LoadProjectFile(m_currentProjectFilePath))
                return fail("Could not validate the generated project document.");

            std::string finalProjectPath = NormalizeProjectPath(PathToUtf8(destination));
            std::string finalProjectFilePath =
                NormalizeProjectPath(PathToUtf8(destination / PathFromUtf8(projectName + ".sparkproject")));
            fs::rename(staging, destination);
            committed = true;
            ownsStaging = false;
            ProjectInfo createdProject = std::move(m_currentProject);
            createdProject.path = std::move(finalProjectPath);
            std::string createdProjectFilePath = std::move(finalProjectFilePath);

            // Project generation is staged in m_currentProject while the old
            // project remains logically open. Once the filesystem commit is
            // irreversible, retire the previous project before publishing the
            // replacement so lifecycle subscribers see the same close/open
            // sequence as OpenProject().
            m_currentProject = previousProject;
            m_currentProjectFilePath = previousProjectFilePath;
            m_hasOpenProject = previouslyOpen;
            if (previouslyOpen)
                CloseProject();

            m_currentProject = std::move(createdProject);
            m_currentProjectFilePath = std::move(createdProjectFilePath);
            m_hasOpenProject = true;
            try
            {
                std::lock_guard lock(s_activeProjectMutex);
                s_activeProjectPath = m_currentProject.path;
            }
            catch (const std::exception& activePathError)
            {
                std::cerr << "Could not publish active project path after commit: " << activePathError.what() << "\n";
            }
            try
            {
                AddToRecentProjects(projectName, GetProjectFilePath());
            }
            catch (const std::exception& recentError)
            {
                std::cerr << "Could not update recent projects after commit: " << recentError.what() << "\n";
            }

            if (m_onProjectOpened)
            {
                try
                {
                    m_onProjectOpened(m_currentProject);
                }
                catch (const std::exception& callbackError)
                {
                    std::cerr << "Project-opened callback failed after commit: " << callbackError.what() << "\n";
                }
                catch (...)
                {
                    std::cerr << "Project-opened callback failed after commit.\n";
                }
            }

            std::cout << "Project created successfully: " << PathToUtf8(destination) << "\n";
            return true;
        }
        catch (const std::exception& e)
        {
            return fail("Error creating project: " + std::string(e.what()));
        }
    }

    bool ProjectManager::CreateProjectFromTemplate(const std::string& projectName, const std::string& projectPath,
                                                   const std::string& templateName)
    {
        try
        {
            return CreateProjectFromTemplateInternal(projectName, projectPath, templateName, "", "");
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error preparing template project creation: " << e.what() << "\n";
            return false;
        }
    }

    bool ProjectManager::CreateProjectFromTemplateInternal(const std::string& projectName,
                                                           const std::string& projectPath,
                                                           const std::string& templateName,
                                                           const std::string& description,
                                                           const std::string& resolvedTemplateRoot)
    {
        if (!IsPortableProjectName(projectName))
        {
            std::cerr << "Project name is not a portable filesystem component: " << projectName << "\n";
            return false;
        }
        if (!IsPortableProjectName(templateName))
        {
            std::cerr << "Template package name is not a portable filesystem component: " << templateName << "\n";
            return false;
        }
        std::cout << "Creating project '" << projectName << "' from template '" << templateName << "' at "
                  << projectPath << "\n";

        const std::string templateRoot =
            resolvedTemplateRoot.empty() ? ResolveTemplateRoot(m_engineRoot) : resolvedTemplateRoot;
        if (templateRoot.empty())
        {
            std::cerr << "No valid SparkEngine template root was found for package: " << templateName << "\n";
            return false;
        }

        std::error_code templateEc;
        const fs::path canonicalTemplateRoot = fs::weakly_canonical(PathFromUtf8(templateRoot), templateEc);
        if (templateEc)
        {
            std::cerr << "Template root could not be canonicalized: " << templateRoot << "\n";
            return false;
        }
        templateEc.clear();
        const fs::path canonicalTemplateDir =
            fs::weakly_canonical(canonicalTemplateRoot / PathFromUtf8(templateName), templateEc);
        if (templateEc)
        {
            std::cerr << "Template package could not be canonicalized: " << templateName << "\n";
            return false;
        }
        const auto mismatchResult = std::mismatch(canonicalTemplateRoot.begin(), canonicalTemplateRoot.end(),
                                                  canonicalTemplateDir.begin(), canonicalTemplateDir.end());
        const bool contained = mismatchResult.first == canonicalTemplateRoot.end();
        const std::string templateDir = PathToUtf8(canonicalTemplateDir);
        templateEc.clear();
        const bool isTemplateDirectory = fs::is_directory(canonicalTemplateDir, templateEc);
        if (templateEc || !contained || !isTemplateDirectory)
        {
            std::cerr << "Template not found: " << templateDir << "\n";
            return false;
        }

        const ProjectInfo previousProject = m_currentProject;
        const std::string previousProjectFilePath = m_currentProjectFilePath;
        const bool previouslyOpen = m_hasOpenProject;
        std::error_code destinationEc;
        const fs::path destination = fs::absolute(PathFromUtf8(projectPath), destinationEc).lexically_normal();
        if (destinationEc || destination.filename().empty())
        {
            std::cerr << "Project destination path is invalid: " << projectPath << "\n";
            return false;
        }
        fs::path staging;
        bool ownsStaging = false;
        bool committed = false;

        auto fail = [&](const std::string& message)
        {
            if (!committed)
            {
                if (ownsStaging)
                {
                    std::error_code cleanupEc;
                    fs::remove_all(staging, cleanupEc);
                    if (cleanupEc)
                        std::cerr << "Could not remove failed project staging directory '" << PathToUtf8(staging)
                                  << "': " << cleanupEc.message() << "\n";
                }
                m_currentProject = previousProject;
                m_currentProjectFilePath = previousProjectFilePath;
                m_hasOpenProject = previouslyOpen;
            }
            if (!message.empty())
                std::cerr << message << "\n";
            return false;
        };

        try
        {
            if (fs::exists(destination))
                return fail("Project destination already exists: " + PathToUtf8(destination));

            if (TemplateTreeContainsLinks(canonicalTemplateDir))
                return fail("Template package contains a symbolic link or reparse point: " + templateDir);

            staging = CreateOwnedStagingDirectory(destination);
            if (staging.empty())
                return fail("Could not create an isolated project staging directory.");
            ownsStaging = true;

            if (!CopyTemplate(templateDir, PathToUtf8(staging), projectName))
                return fail("Could not copy template package into staging directory.");

            const fs::path canonicalProjectFile = staging / PathFromUtf8(projectName + ".sparkproject");
            const fs::path legacyProjectFile = staging / "spark.project.json";
            const fs::path sourceProjectFile =
                fs::is_regular_file(canonicalProjectFile) ? canonicalProjectFile : legacyProjectFile;
            if (fs::is_regular_file(sourceProjectFile))
            {
                if (!LoadProjectFile(PathToUtf8(sourceProjectFile)))
                    return fail("Could not load the template project metadata.");
            }
            else
                m_currentProject = ProjectInfo{};

            m_currentProject.name = projectName;
            m_currentProject.path = NormalizeProjectPath(PathToUtf8(staging));
            m_currentProjectFilePath = NormalizeProjectPath(PathToUtf8(canonicalProjectFile));
            if (m_currentProject.version.empty())
                m_currentProject.version = "1.0.0";
            if (m_currentProject.engineVersion.empty())
                m_currentProject.engineVersion = GetCurrentEngineVersion().ToString();
            if (m_currentProject.createdTime == 0)
                m_currentProject.createdTime = GetCurrentTimestamp();
            m_currentProject.lastModified = GetCurrentTimestamp();
            m_currentProject.modules = {MakeCodeIdentifier(projectName)};

            if (!EnsureBuildScaffold(PathToUtf8(staging), projectName))
                return fail("Could not create the project build scaffold.");

            if (const auto* descriptor = FindProjectTemplateDescriptor(templateName))
            {
                const std::vector<std::string> declaredScenes = m_currentProject.scenes;
                const fs::path defaultScene = staging / fs::path(descriptor->defaultScene);
                if (!fs::is_regular_file(defaultScene))
                    return fail("Template package is missing its declared default scene: " + PathToUtf8(defaultScene));
                m_currentProject.templateType = descriptor->type;
                m_currentProject.hasTemplateIdentity = true;
                m_currentProject.description = description.empty() ? std::string(descriptor->description) : description;
                m_currentProject.defaultScene = std::string(descriptor->defaultScene);
                m_currentProject.lastOpenedScene = m_currentProject.defaultScene;
                m_currentProject.scenes = {m_currentProject.defaultScene};
                for (const std::string& declaredScene : declaredScenes)
                {
                    fs::path relative = PathFromUtf8(declaredScene).lexically_normal();
                    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                        return fail("Template project declares an unsafe scene path: " + declaredScene);
                    const fs::path candidate = staging / relative;
                    if (!fs::is_regular_file(candidate))
                        return fail("Template project is missing its declared scene: " + PathToUtf8(candidate));
                    std::string storedPath = PathToUtf8(relative);
                    std::replace(storedPath.begin(), storedPath.end(), '\\', '/');
                    if (std::none_of(m_currentProject.scenes.begin(), m_currentProject.scenes.end(),
                                     [&](const std::string& scene)
                                     {
                                         return ProjectPathsEqual(PathToUtf8(staging / PathFromUtf8(scene)),
                                                                  PathToUtf8(candidate));
                                     }))
                        m_currentProject.scenes.push_back(std::move(storedPath));
                }
            }
            else if (m_currentProject.description.empty())
            {
                m_currentProject.description = description.empty() ? "Spark Engine Project" : description;
            }

            if (!SaveProjectFile())
                return fail("Could not write the canonical project document.");
            if (!LoadProjectFile(PathToUtf8(canonicalProjectFile)))
                return fail("Could not validate the canonical project document.");
            if (legacyProjectFile != canonicalProjectFile)
            {
                std::error_code removeEc;
                const bool removed = fs::remove(legacyProjectFile, removeEc);
                if (removeEc || (fs::exists(legacyProjectFile) && !removed))
                    return fail("Could not remove the legacy project document from staging.");
            }

            std::string finalProjectPath = NormalizeProjectPath(PathToUtf8(destination));
            std::string finalProjectFilePath =
                NormalizeProjectPath(PathToUtf8(destination / PathFromUtf8(projectName + ".sparkproject")));
            fs::rename(staging, destination);
            committed = true;
            ownsStaging = false;
            ProjectInfo createdProject = std::move(m_currentProject);
            createdProject.path = std::move(finalProjectPath);
            std::string createdProjectFilePath = std::move(finalProjectFilePath);

            // Match OpenProject() replacement semantics: after the staged
            // tree is committed, close the old logical project exactly once
            // before publishing the newly created one.
            m_currentProject = previousProject;
            m_currentProjectFilePath = previousProjectFilePath;
            m_hasOpenProject = previouslyOpen;
            if (previouslyOpen)
                CloseProject();

            m_currentProject = std::move(createdProject);
            m_currentProjectFilePath = std::move(createdProjectFilePath);
            m_hasOpenProject = true;
            try
            {
                std::lock_guard lock(s_activeProjectMutex);
                s_activeProjectPath = m_currentProject.path;
            }
            catch (const std::exception& activePathError)
            {
                std::cerr << "Could not publish active project path after commit: " << activePathError.what() << "\n";
            }
            try
            {
                AddToRecentProjects(projectName, GetProjectFilePath());
            }
            catch (const std::exception& recentError)
            {
                std::cerr << "Could not update recent projects after commit: " << recentError.what() << "\n";
            }

            if (m_onProjectOpened)
            {
                try
                {
                    m_onProjectOpened(m_currentProject);
                }
                catch (const std::exception& callbackError)
                {
                    std::cerr << "Project-opened callback failed after commit: " << callbackError.what() << "\n";
                }
                catch (...)
                {
                    std::cerr << "Project-opened callback failed after commit.\n";
                }
            }

            std::cout << "Project '" << projectName << "' created successfully from template '" << templateName
                      << "'\n";
            return true;
        }
        catch (const std::exception& e)
        {
            return fail("Error creating project from template: " + std::string(e.what()));
        }
    }

    bool ProjectManager::CopyTemplate(const std::string& templatePath, const std::string& destPath,
                                      const std::string& projectName)
    {
        try
        {
            const fs::path sourceRoot = PathFromUtf8(templatePath);
            const fs::path destinationRoot = PathFromUtf8(destPath);
            if (!fs::is_directory(sourceRoot) || !fs::is_directory(destinationRoot) || !fs::is_empty(destinationRoot) ||
                TemplateTreeContainsLinks(sourceRoot))
            {
                std::cerr << "Template copy requires a link-free source and an empty owned destination.\n";
                return false;
            }
            for (const auto& source : fs::directory_iterator(sourceRoot))
            {
                fs::copy(source.path(), destinationRoot / source.path().filename(),
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            }

            // Text file extensions that should have their template name rewritten
            static const std::set<std::string> textExtensions = {
                ".h", ".hpp", ".cpp", ".c", ".txt", ".json", ".cmake", ".md", ".py", ".sparkproject", ".sparkscene"};

            // Templates are shipped as real, compilable game modules named after
            // their directory (e.g. `Templates/FPSStarter` uses `FPSStarterModule`,
            // project name `FPSStarter`, CMake target `FPSStarter`, etc.). When we
            // materialize a user project from a template, rewrite every textual
            // occurrence of the template's name to the user's chosen project name
            // so they do not have to do it by hand.
            const std::string templateName = PathToUtf8(sourceRoot.filename());
            const std::string codeIdentifier = MakeCodeIdentifier(projectName);
            if (templateName.empty())
            {
                return true;
            }

            std::vector<std::pair<fs::path, fs::path>> renames;
            for (auto& entry : fs::recursive_directory_iterator(destinationRoot))
            {
                if (!entry.is_regular_file())
                    continue;

                const std::string filename = PathToUtf8(entry.path().filename());
                if (const size_t token = filename.find(templateName); token != std::string::npos)
                {
                    std::string renamed = filename;
                    renamed.replace(token, templateName.size(), projectName);
                    renames.emplace_back(entry.path(), entry.path().parent_path() / PathFromUtf8(renamed));
                }

                auto ext = entry.path().extension().string();
                if (!Spark::ContainerUtils::Contains(textExtensions, ext))
                    continue;

                std::ifstream inFile(entry.path());
                if (!inFile.is_open())
                    return false;

                std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
                if (inFile.bad())
                    return false;
                inFile.close();

                if (!content.contains(templateName))
                    continue;

                size_t pos = 0;
                while ((pos = content.find(templateName, pos)) != std::string::npos)
                {
                    // Package tokens participate in C++ symbols, CMake targets,
                    // and module binary stems. Never substitute the UTF-8
                    // display name into those identifier-bearing surfaces.
                    content.replace(pos, templateName.length(), codeIdentifier);
                    pos += codeIdentifier.length();
                }

                std::ofstream outFile(entry.path());
                if (!outFile.is_open())
                    return false;
                outFile << content;
                outFile.flush();
                if (!outFile.good())
                    return false;
                outFile.close();
                if (outFile.fail())
                    return false;
            }

            for (const auto& [source, destination] : renames)
            {
                if (source != destination)
                    fs::rename(source, destination);
            }

            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error copying template: " << e.what() << "\n";
            return false;
        }
    }

    bool ProjectManager::OpenProject(const std::string& sparkprojectPath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !sparkprojectPath.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading project from '%s'", sparkprojectPath.c_str());
        std::cout << "Opening project: " << sparkprojectPath << "\n";

        const bool hadOpenProject = m_hasOpenProject;
        const ProjectInfo previousProject = m_currentProject;
        const std::string previousProjectFilePath = m_currentProjectFilePath;
        bool transitionCommitted = false;

        const auto restorePreviousProject = [&]()
        {
            m_hasOpenProject = hadOpenProject;
            m_currentProject = previousProject;
            m_currentProjectFilePath = previousProjectFilePath;
        };

        try
        {

            // Accept either a .sparkproject file or a directory containing one
            std::string resolvedPath = sparkprojectPath;
            const fs::path requestedPath = PathFromUtf8(sparkprojectPath);
            std::error_code pathEc;
            if (fs::is_directory(requestedPath, pathEc) && !pathEc)
            {
                // Search for .sparkproject file inside
                for (auto& entry : fs::directory_iterator(requestedPath))
                {
                    if (entry.path().extension() == ".sparkproject")
                    {
                        resolvedPath = PathToUtf8(entry.path());
                        break;
                    }
                }
                // Also try spark.project.json (new module format)
                if (resolvedPath == sparkprojectPath)
                {
                    const fs::path sparkJson = requestedPath / "spark.project.json";
                    if (fs::exists(sparkJson))
                        resolvedPath = PathToUtf8(sparkJson);
                }
            }

            if (!fs::is_regular_file(PathFromUtf8(resolvedPath)))
            {
                std::cerr << "Project file not found: " << resolvedPath << "\n";
                return false;
            }

            if (!LoadProjectFile(resolvedPath))
            {
                restorePreviousProject();
                return false;
            }

            // Older editor-created projects predate the standalone build skeleton.
            // Add only missing generated files; never overwrite user-authored build
            // scripts or module sources.
            if (!EnsureBuildScaffold(m_currentProject.path, m_currentProject.name))
            {
                restorePreviousProject();
                return false;
            }

            // Loading and scaffold validation above are staged while the old
            // project remains logically open. Only now is it safe to notify the
            // editor that the previous document should be retired.
            ProjectInfo loadedProject = std::move(m_currentProject);
            std::string loadedProjectFilePath = std::move(m_currentProjectFilePath);
            const std::string loadedActiveProjectPath = NormalizeProjectPath(loadedProject.path);
            restorePreviousProject();
            if (hadOpenProject)
                CloseProject();

            m_currentProject = std::move(loadedProject);
            m_currentProjectFilePath = std::move(loadedProjectFilePath);
            m_hasOpenProject = true;
            transitionCommitted = true;
            // Everything below is post-commit bookkeeping. A notification or
            // callback failure must not turn a successful project replacement
            // into a false return or attempt to resurrect the already-retired
            // document.
            try
            {
                std::lock_guard lock(s_activeProjectMutex);
                s_activeProjectPath = loadedActiveProjectPath;
            }
            catch (const std::exception& activePathError)
            {
                std::cerr << "Could not publish active project path after commit: " << activePathError.what() << "\n";
            }

            try
            {
                AddToRecentProjects(m_currentProject.name, resolvedPath);
            }
            catch (const std::exception& recentError)
            {
                std::cerr << "Could not update recent projects after commit: " << recentError.what() << "\n";
            }

            if (m_onProjectOpened)
            {
                try
                {
                    m_onProjectOpened(m_currentProject);
                }
                catch (const std::exception& callbackError)
                {
                    std::cerr << "Project-opened callback failed after commit: " << callbackError.what() << "\n";
                }
                catch (...)
                {
                    std::cerr << "Project-opened callback failed after commit.\n";
                }
            }

            return true;
        }
        catch (const std::exception& e)
        {
            if (!transitionCommitted)
                restorePreviousProject();
            std::cerr << "Error opening project: " << e.what() << "\n";
            return false;
        }
    }

    bool ProjectManager::SaveProject()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!m_hasOpenProject)
        {
            std::cerr << "No project is currently open\n";
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving project '%s'", m_currentProject.name.c_str());
        std::cout << "Saving project: " << m_currentProject.name << "\n";
        m_currentProject.lastModified = GetCurrentTimestamp();
        return SaveProjectFile();
    }

    bool ProjectManager::ResolveProjectScenePath(const std::string& scenePath, std::string& resolvedPath) const
    {
        resolvedPath.clear();
        if (!m_hasOpenProject || scenePath.empty())
            return false;

        const fs::path projectRoot = PathFromUtf8(NormalizeProjectPath(m_currentProject.path));
        fs::path candidate = PathFromUtf8(scenePath);
        if (candidate.is_relative())
            candidate = projectRoot / candidate;
        candidate = PathFromUtf8(NormalizeProjectPath(PathToUtf8(candidate)));
        if (!fs::is_regular_file(candidate))
            return false;

        std::error_code ec;
        const fs::path relative = fs::relative(candidate, projectRoot, ec).lexically_normal();
        if (ec || relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            return false;

        resolvedPath = PathToUtf8(candidate);
        return true;
    }

    bool ProjectManager::LoadProjectScene(const std::string& scenePath, ::World& world, std::string& resolvedPath) const
    {
        resolvedPath.clear();
        if (!m_hasOpenProject || scenePath.empty())
            return false;

        const fs::path projectRoot = PathFromUtf8(NormalizeProjectPath(m_currentProject.path));
        fs::path candidate = PathFromUtf8(scenePath);
        if (candidate.is_relative())
            candidate = projectRoot / candidate;
        candidate = fs::absolute(candidate).lexically_normal();

        // Reject obvious absolute/traversal escapes before opening anything.
        // The handle-derived final path below remains the security authority
        // for symlinks, junctions, and concurrent path replacement.
        if (!IsPathInsideRoot(projectRoot, candidate))
            return false;

        std::string sceneDocument;
        if (!ReadContainedFileFromHandle(projectRoot, candidate, resolvedPath, sceneDocument))
            return false;
        if (!Spark::DeserializeInto(world, sceneDocument))
        {
            resolvedPath.clear();
            return false;
        }
        return true;
    }

    bool ProjectManager::RecordOpenedScene(const std::string& scenePath)
    {
        std::string resolvedPath;
        if (!ResolveProjectScenePath(scenePath, resolvedPath))
            return false;

        const fs::path projectRoot = PathFromUtf8(NormalizeProjectPath(m_currentProject.path));
        const fs::path candidate = PathFromUtf8(resolvedPath);

        std::error_code ec;
        const fs::path relative = fs::relative(candidate, projectRoot, ec).lexically_normal();
        if (ec || relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            return false;

        std::string storedPath = PathToUtf8(relative);
        std::replace(storedPath.begin(), storedPath.end(), '\\', '/');
        const bool sceneAlreadyRecorded = std::any_of(
            m_currentProject.scenes.begin(), m_currentProject.scenes.end(), [&](const std::string& scene)
            { return ProjectPathsEqual(PathToUtf8(projectRoot / PathFromUtf8(scene)), PathToUtf8(candidate)); });

        // Opening the project's already-current scene is a read-only operation.
        // Avoid rewriting checked-in template descriptors (and dirtying their
        // lastModified field) every time the editor starts.
        if (ProjectPathsEqual(PathToUtf8(projectRoot / PathFromUtf8(m_currentProject.lastOpenedScene)),
                              PathToUtf8(candidate)))
        {
            return true;
        }

        const ProjectInfo previous = m_currentProject;
        m_currentProject.lastOpenedScene = storedPath;
        if (!sceneAlreadyRecorded)
        {
            m_currentProject.scenes.push_back(storedPath);
        }
        m_currentProject.lastModified = GetCurrentTimestamp();
        if (SaveProjectFile())
            return true;

        m_currentProject = previous;
        return false;
    }

    void ProjectManager::CloseProject()
    {
        if (m_hasOpenProject)
        {
            std::cout << "Closing project: " << m_currentProject.name << "\n";
            ProjectInfo closed = m_currentProject;
            m_hasOpenProject = false;
            m_currentProject = ProjectInfo{};
            m_currentProjectFilePath.clear();
            {
                std::lock_guard lock(s_activeProjectMutex);
                if (ProjectPathsEqual(s_activeProjectPath, closed.path))
                    s_activeProjectPath.clear();
            }
            if (m_onProjectClosed)
            {
                try
                {
                    m_onProjectClosed(closed);
                }
                catch (const std::exception& callbackError)
                {
                    std::cerr << "Project-closed callback failed after close: " << callbackError.what() << "\n";
                }
                catch (...)
                {
                    std::cerr << "Project-closed callback failed after close.\n";
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Path helpers
    // ------------------------------------------------------------------
    std::string ProjectManager::GetProjectAssetsPath() const
    {
        return PathToUtf8(PathFromUtf8(m_currentProject.path) / "Assets");
    }
    std::string ProjectManager::GetProjectScenesPath() const
    {
        return PathToUtf8(PathFromUtf8(m_currentProject.path) / "Scenes");
    }
    std::string ProjectManager::GetProjectScriptsPath() const
    {
        return PathToUtf8(PathFromUtf8(m_currentProject.path) / "Scripts");
    }
    std::string ProjectManager::GetProjectConfigPath() const
    {
        return PathToUtf8(PathFromUtf8(m_currentProject.path) / "Config");
    }
    std::string ProjectManager::GetProjectTempPath() const
    {
        return PathToUtf8(PathFromUtf8(m_currentProject.path) / "Temp");
    }
    std::string ProjectManager::GetProjectFilePath() const
    {
        if (!m_currentProjectFilePath.empty())
            return m_currentProjectFilePath;
        if (m_currentProject.path.empty() || m_currentProject.name.empty())
            return {};
        return NormalizeProjectPath(
            PathToUtf8(PathFromUtf8(m_currentProject.path) / PathFromUtf8(m_currentProject.name + ".sparkproject")));
    }

    std::string ProjectManager::GetActiveProjectPath()
    {
        std::lock_guard lock(s_activeProjectMutex);
        return s_activeProjectPath;
    }

    // ------------------------------------------------------------------
    // Recent projects
    // ------------------------------------------------------------------
    void ProjectManager::RefreshRecentProjects()
    {
        std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
        for (auto& rp : m_recentProjects)
        {
            rp.valid = fs::exists(rp.path);
        }
    }

    void ProjectManager::RemoveRecentProject(const std::string& path)
    {
        const std::string normalizedPath = NormalizeProjectPath(path);
        {
            std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
            m_recentProjects.erase(std::remove_if(m_recentProjects.begin(), m_recentProjects.end(),
                                                  [&](const RecentProject& rp)
                                                  { return ProjectPathsEqual(rp.path, normalizedPath); }),
                                   m_recentProjects.end());
        }
        SaveRecentProjectsList();
    }

    void ProjectManager::ClearRecentProjects()
    {
        {
            std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
            m_recentProjects.clear();
        }
        SaveRecentProjectsList();
    }

    void ProjectManager::AddToRecentProjects(const std::string& projectName, const std::string& sparkprojectPath)
    {
        const std::string normalizedPath = NormalizeProjectPath(sparkprojectPath);
        {
            std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
            // Remove existing entry with same path
            m_recentProjects.erase(std::remove_if(m_recentProjects.begin(), m_recentProjects.end(),
                                                  [&](const RecentProject& rp)
                                                  { return ProjectPathsEqual(rp.path, normalizedPath); }),
                                   m_recentProjects.end());

            RecentProject rp;
            rp.name = projectName;
            rp.path = normalizedPath;
            rp.engineVersion = GetCurrentEngineVersion().ToString();
            rp.lastOpened = GetCurrentTimestamp();
            rp.valid = true;

            m_recentProjects.insert(m_recentProjects.begin(), rp);

            // Keep max 10
            if (m_recentProjects.size() > 10)
            {
                m_recentProjects.resize(10);
            }
        }
        SaveRecentProjectsList();
    }

    // ------------------------------------------------------------------
    // Project file I/O (.sparkproject)
    // ------------------------------------------------------------------
    bool ProjectManager::LoadProjectFile(const std::string& sparkprojectPath)
    {
        std::string content;

        if (m_fileCache)
        {
            auto result = m_fileCache->ReadText(sparkprojectPath);
            if (result.IsOk())
            {
                content = result.Value();
            }
        }

        if (content.empty())
        {
            std::ifstream file(PathFromUtf8(sparkprojectPath));
            if (!file.is_open())
            {
                std::cerr << "Could not open project file: " << sparkprojectPath << "\n";
                return false;
            }
            content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();
        }

        std::string name = ExtractJsonString(content, "name");
        std::string version = ExtractJsonString(content, "version");
        std::string description = ExtractJsonString(content, "description");
        std::string engineVer = ExtractJsonString(content, "engineVersion");
        std::string templateId = ExtractJsonString(content, "template");
        std::string defaultScene = ExtractJsonString(content, "defaultScene");
        std::string lastScene = ExtractJsonString(content, "lastOpenedScene");
        uint64_t lastModified = ExtractJsonUint64(content, "lastModified");
        uint64_t createdTime = ExtractJsonUint64(content, "createdTime");
        auto scenes = ExtractJsonStringArray(content, "scenes");
        auto modules = ExtractJsonStringArray(content, "modules");

        // Preserve the selected document path independently from its display
        // name. On macOS, normalization also resolves /var -> /private/var;
        // every public project path then uses the same canonical spelling.
        const std::string normalizedProjectFile = NormalizeProjectPath(sparkprojectPath);
        const fs::path projectRootPath = PathFromUtf8(normalizedProjectFile).parent_path();
        std::string projectRoot = PathToUtf8(projectRootPath);

        m_currentProject = ProjectInfo{};
        m_currentProjectFilePath = normalizedProjectFile;
        m_currentProject.name = name.empty() ? PathToUtf8(projectRootPath.filename()) : name;
        m_currentProject.path = projectRoot;
        m_currentProject.version = version.empty() ? "1.0.0" : version;
        m_currentProject.description = description.empty() ? "Spark Engine Project" : description;
        m_currentProject.engineVersion = engineVer.empty() ? GetCurrentEngineVersion().ToString() : engineVer;
        m_currentProject.defaultScene = defaultScene;
        m_currentProject.lastOpenedScene = lastScene;
        m_currentProject.lastModified = lastModified;
        m_currentProject.createdTime = createdTime;
        m_currentProject.scenes = scenes;
        m_currentProject.modules = modules;
        if (const auto* descriptor = FindProjectTemplateDescriptor(templateId))
        {
            m_currentProject.templateType = descriptor->type;
            m_currentProject.hasTemplateIdentity = true;
        }

        std::cout << "Loaded project: " << m_currentProject.name << " v" << m_currentProject.version << " (engine "
                  << m_currentProject.engineVersion << ")\n";
        return true;
    }

    bool ProjectManager::SaveProjectFile()
    {
        std::string filePath = GetProjectFilePath();

        try
        {
            // Ensure directory exists
            const fs::path nativeFilePath = PathFromUtf8(filePath);
            fs::create_directories(nativeFilePath.parent_path());

            std::ofstream file(nativeFilePath);
            if (!file.is_open())
            {
                std::cerr << "Failed to open project file for writing: " << filePath << "\n";
                return false;
            }

            file << "{\n";
            file << "  \"projectFileVersion\": 1,\n";
            file << "  \"name\": \"" << EscapeJsonString(m_currentProject.name) << "\",\n";
            file << "  \"version\": \"" << EscapeJsonString(m_currentProject.version) << "\",\n";
            file << "  \"description\": \"" << EscapeJsonString(m_currentProject.description) << "\",\n";
            file << "  \"engineVersion\": \"" << EscapeJsonString(m_currentProject.engineVersion) << "\",\n";
            if (m_currentProject.hasTemplateIdentity)
                if (const auto* descriptor = FindProjectTemplateDescriptor(m_currentProject.templateType))
                    file << "  \"template\": \"" << EscapeJsonString(std::string(descriptor->stableId)) << "\",\n";
            file << "  \"defaultScene\": \"" << EscapeJsonString(m_currentProject.defaultScene) << "\",\n";
            file << "  \"lastOpenedScene\": \"" << EscapeJsonString(m_currentProject.lastOpenedScene) << "\",\n";
            file << "  \"createdTime\": " << m_currentProject.createdTime << ",\n";
            file << "  \"lastModified\": " << m_currentProject.lastModified << ",\n";

            // modules array
            file << "  \"modules\": [\n";
            for (size_t i = 0; i < m_currentProject.modules.size(); ++i)
            {
                file << "    \"" << EscapeJsonString(m_currentProject.modules[i]) << "\"";
                if (i + 1 < m_currentProject.modules.size())
                    file << ",";
                file << "\n";
            }
            file << "  ],\n";

            // scenes array
            file << "  \"scenes\": [\n";
            for (size_t i = 0; i < m_currentProject.scenes.size(); ++i)
            {
                file << "    \"" << EscapeJsonString(m_currentProject.scenes[i]) << "\"";
                if (i + 1 < m_currentProject.scenes.size())
                    file << ",";
                file << "\n";
            }
            file << "  ]\n";
            file << "}\n";

            file.flush();
            if (!file.good())
            {
                std::cerr << "Failed while writing project file: " << filePath << "\n";
                return false;
            }
            file.close();
            if (file.fail())
            {
                std::cerr << "Failed while closing project file: " << filePath << "\n";
                return false;
            }

            if (m_fileCache)
            {
                m_fileCache->Invalidate(filePath);
            }

            std::cout << "Project file saved: " << filePath << "\n";
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error saving project file: " << e.what() << "\n";
            return false;
        }
    }

    // ------------------------------------------------------------------
    // Project structure creation
    // ------------------------------------------------------------------
    bool ProjectManager::CreateProjectStructure(const std::string& projectPath, const std::string& projectName,
                                                ProjectTemplate templateType)
    {
        try
        {
            const fs::path root = PathFromUtf8(projectPath);
            fs::create_directories(root);

            // Core directories every project needs
            for (const fs::path& directory :
                 {root / "Source", root / "Assets", root / "Assets/Textures", root / "Assets/Models",
                  root / "Assets/Materials", root / "Assets/Audio", root / "Assets/Fonts", root / "Assets/Prefabs",
                  root / "Scenes", root / "Scripts", root / "Config", root / "Temp", root / "Saved",
                  root / "Saved/Backups"})
                fs::create_directories(directory);

            // Template-specific directories
            if (templateType == ProjectTemplate::FirstPerson || templateType == ProjectTemplate::ThirdPerson)
            {
                fs::create_directories(root / "Assets/Characters");
                fs::create_directories(root / "Assets/Weapons");
                fs::create_directories(root / "Assets/UI");
            }

            // Create a scene through the same serializer the editor uses to
            // open/save it. Hand-written legacy JSON here used a different
            // schema and appeared empty as soon as the project opened.
            const auto* descriptor = FindProjectTemplateDescriptor(templateType);
            const fs::path scenePath =
                root / (descriptor ? fs::path(descriptor->defaultScene) : fs::path("Scenes/Default.sparkscene"));
            fs::create_directories(scenePath.parent_path());
            if (!CreateDefaultScene(PathToUtf8(scenePath), templateType))
                return false;

            // Create editor settings
            CreateDefaultEditorSettings(PathToUtf8(root / "Config"));

            if (!EnsureBuildScaffold(projectPath, projectName))
                return false;

            // Create .gitignore for the project
            {
                std::ofstream gitignore(root / ".gitignore");
                if (gitignore.is_open())
                {
                    gitignore << "# Spark Engine Project\n";
                    gitignore << "Temp/\n";
                    gitignore << "Saved/Backups/\n";
                    gitignore << "*.log\n";
                    gitignore << ".vs/\n";
                    gitignore << "build/\n";
                    gitignore.close();
                }
            }

            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error creating project structure: " << e.what() << "\n";
            return false;
        }
    }

    bool ProjectManager::CreateDefaultScene(const std::string& scenePath, ProjectTemplate templateType)
    {
        ::World world;

        const ::EntityID lightEntity = world.CreateEntity("Directional Light");
        auto& lightTransform = world.AddComponent<::Transform>(lightEntity);
        lightTransform.position = {0.0f, 10.0f, 0.0f};
        lightTransform.rotation = {50.0f, -30.0f, 0.0f};
        auto& light = world.AddComponent<::LightComponent>(lightEntity);
        light.type = ::LightComponent::Type::Directional;
        light.color = {1.0f, 0.96f, 0.84f};
        light.intensity = 1.0f;

        if (templateType != ProjectTemplate::Empty)
        {
            const ::EntityID groundEntity = world.CreateEntity("Ground");
            auto& groundTransform = world.AddComponent<::Transform>(groundEntity);
            groundTransform.scale = {50.0f, 0.1f, 50.0f};
            auto& groundMesh = world.AddComponent<::MeshRenderer>(groundEntity);
            // A non-empty missing path deliberately uses the renderer's
            // procedural placeholder mesh, giving every fresh project visible
            // geometry without depending on engine-repository assets.
            groundMesh.meshPath = "__spark_primitive_ground__.obj";
        }

        if (templateType != ProjectTemplate::Empty)
        {
            const ::EntityID cameraEntity = world.CreateEntity("Main Camera");
            auto& cameraTransform = world.AddComponent<::Transform>(cameraEntity);
            switch (templateType)
            {
            case ProjectTemplate::FirstPerson:
                cameraTransform.position = {0.0f, 1.7f, -8.0f};
                break;
            case ProjectTemplate::ThirdPerson:
                cameraTransform.position = {0.0f, 5.0f, -8.0f};
                cameraTransform.rotation = {25.0f, 0.0f, 0.0f};
                break;
            case ProjectTemplate::TopDown:
                cameraTransform.position = {0.0f, 20.0f, 0.0f};
                cameraTransform.rotation = {90.0f, 0.0f, 0.0f};
                break;
            case ProjectTemplate::Platformer:
                cameraTransform.position = {0.0f, 5.0f, -12.0f};
                cameraTransform.rotation = {10.0f, 0.0f, 0.0f};
                break;
            case ProjectTemplate::RPG:
                cameraTransform.position = {0.0f, 8.0f, -10.0f};
                cameraTransform.rotation = {30.0f, 0.0f, 0.0f};
                break;
            default:
                cameraTransform.position = {0.0f, 3.0f, -8.0f};
                cameraTransform.rotation = {15.0f, 0.0f, 0.0f};
                break;
            }
            auto& camera = world.AddComponent<::Camera>(cameraEntity);
            camera.isMainCamera = true;
        }

        if (templateType == ProjectTemplate::FirstPerson || templateType == ProjectTemplate::ThirdPerson)
        {
            const ::EntityID playerEntity = world.CreateEntity("Player");
            auto& playerTransform = world.AddComponent<::Transform>(playerEntity);
            playerTransform.position = {0.0f, 1.0f, 0.0f};
            world.AddComponent<::CharacterControllerComponent>(playerEntity);
        }

        return Spark::SaveWorld(world, scenePath);
    }

    bool ProjectManager::EnsureBuildScaffold(const std::string& projectPath, const std::string& projectName)
    {
        try
        {
            const fs::path root = fs::absolute(PathFromUtf8(projectPath)).lexically_normal();
            const fs::path source = root / "Source";
            fs::create_directories(source);

            const std::string target = MakeCodeIdentifier(projectName);
            const std::string displayName = EscapeCppString(projectName);

            std::ostringstream cmake;
            cmake << "cmake_minimum_required(VERSION 3.25)\n"
                  << "project(" << target << " LANGUAGES CXX)\n\n"
                  << "set(CMAKE_CXX_STANDARD 23)\n"
                  << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n"
                  << "find_package(SparkEngine CONFIG REQUIRED)\n\n"
                  << "file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS\n"
                  << "    \"Source/*.cpp\"\n"
                  << "    \"Source/*.h\"\n"
                  << "    \"Source/*.hpp\")\n\n"
                  << "spark_add_game_module(" << target << " ${GAME_SOURCES})\n"
                  << "target_include_directories(" << target << " PRIVATE \"Source\")\n";

            std::ostringstream header;
            header << "#pragma once\n\n"
                   << "#include <Spark/SparkSDK.h>\n"
                   << "#include <Graphics/GraphicsEngine.h>\n\n"
                   << "class " << target << "Module final : public Spark::IModule\n"
                   << "{\npublic:\n"
                   << "    Spark::ModuleInfo GetModuleInfo() const override\n"
                   << "    {\n"
                   << "        Spark::ModuleInfo info{};\n"
                   << "        info.name = \"" << displayName << "\";\n"
                   << "        info.version = \"1.0.0\";\n"
                   << "        info.sdkVersion = SPARK_SDK_VERSION;\n"
                   << "        info.loadOrder = 1000;\n"
                   << "        return info;\n"
                   << "    }\n\n"
                   << "    bool OnLoad(Spark::IEngineContext* context) override\n"
                   << "    {\n        m_context = context;\n        return true;\n    }\n\n"
                   << "    void OnUnload() override { m_context = nullptr; }\n"
                   << "    void OnUpdate(float deltaTime) override { (void)deltaTime; }\n"
                   << "    void OnRender() override\n"
                   << "    {\n"
                   << "        // Modules own their graphics frame.  Present a valid clear frame\n"
                   << "        // even before the project adds its first renderer.\n"
                   << "        if (m_context)\n"
                   << "            if (auto* graphics = m_context->GetGraphics())\n"
                   << "            {\n"
                   << "                graphics->BeginFrame();\n"
                   << "                graphics->EndFrame();\n"
                   << "            }\n"
                   << "    }\n\n"
                   << "private:\n"
                   << "    Spark::IEngineContext* m_context = nullptr;\n"
                   << "};\n";

            std::ostringstream implementation;
            implementation << "#include \"GameModule.h\"\n"
                           << "#include <Spark/ModuleDllMain.h>\n\n"
                           << "SPARK_IMPLEMENT_MODULE(" << target << "Module)\n";

            std::ostringstream modules;
            modules << "{\n  \"modules\": [\n    {\n"
                    << "      \"name\": \"" << EscapeJsonString(target) << "\",\n"
                    << "      \"path\": \"" << EscapeJsonString(target) << ".dll\",\n"
                    << "      \"loadOrder\": 1000\n"
                    << "    }\n  ]\n}\n";

            return WriteNewFile(root / "CMakeLists.txt", cmake.str()) &&
                   WriteNewFile(source / "GameModule.h", header.str()) &&
                   WriteNewFile(source / "GameModule.cpp", implementation.str()) &&
                   WriteNewFile(root / "spark.modules.json", modules.str());
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error creating project build scaffold: " << e.what() << "\n";
            return false;
        }
    }

    void ProjectManager::CreateDefaultEditorSettings(const std::string& configPath)
    {
        std::ofstream file(PathFromUtf8(configPath) / "EditorSettings.json");
        if (!file.is_open())
            return;

        file << "{\n";
        file << "  \"editor\": {\n";
        file << "    \"theme\": \"Spark Professional\",\n";
        file << "    \"fontSize\": 15,\n";
        file << "    \"autoSaveInterval\": 300,\n";
        file << "    \"showGrid\": true,\n";
        file << "    \"gridSize\": 1.0,\n";
        file << "    \"snapToGrid\": false\n";
        file << "  },\n";
        file << "  \"viewport\": {\n";
        file << "    \"fieldOfView\": 60.0,\n";
        file << "    \"nearClip\": 0.1,\n";
        file << "    \"farClip\": 5000.0,\n";
        file << "    \"moveSpeed\": 10.0,\n";
        file << "    \"showGizmos\": true\n";
        file << "  },\n";
        file << "  \"build\": {\n";
        file << "    \"targetPlatform\": \"Windows\",\n";
        file << "    \"configuration\": \"Debug\"\n";
        file << "  }\n";
        file << "}\n";
        file.close();
    }

    // ------------------------------------------------------------------
    // Recent projects persistence
    // ------------------------------------------------------------------
    std::string ProjectManager::GetEditorDataDirectory()
    {
#ifdef _WIN32
        wchar_t appDataPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath)))
        {
            return PathToUtf8(fs::path(appDataPath) / "SparkEngine" / "Editor");
        }
        return "./SparkEditor";
#else
        const char* home = getenv("HOME");
        if (home)
        {
            return std::string(home) + "/.sparkengine/editor";
        }
        return "./.sparkengine/editor";
#endif
    }

    std::string ProjectManager::GetRecentProjectsFilePath()
    {
        return PathToUtf8(PathFromUtf8(GetEditorDataDirectory()) / "RecentProjects.json");
    }

    void ProjectManager::LoadRecentProjectsList()
    {
        std::lock_guard<std::mutex> lock(m_recentProjectsMutex);
        m_recentProjects.clear();
        std::string filePath = GetRecentProjectsFilePath();

        std::string content;

        if (m_fileCache)
        {
            auto result = m_fileCache->ReadText(filePath);
            if (result.IsOk())
            {
                content = result.Value();
            }
        }

        // A recent-projects list is a handful of entries; anything huge is a
        // corrupt/runaway file (a 3 GB one was found in the wild after
        // repeated load/save cycles). Start fresh rather than parse it.
        {
            std::error_code sizeEc;
            const auto sz = fs::file_size(PathFromUtf8(filePath), sizeEc);
            if (!sizeEc && sz > 1024 * 1024)
            {
                std::cerr << "RecentProjects.json is " << sz << " bytes - corrupt/runaway, resetting.\n";
                std::error_code rmEc;
                fs::remove(PathFromUtf8(filePath), rmEc);
                return;
            }
        }

        if (content.empty())
        {
            std::ifstream file(PathFromUtf8(filePath));
            if (!file.is_open())
                return;
            content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();
        }

        // Parse array of recent projects (simple parser)
        // Format: { "recentProjects": [ { "name": ..., "path": ..., ... }, ... ] }
        size_t pos = 0;
        while (true)
        {
            pos = content.find('{', pos);
            if (pos == std::string::npos)
                break;

            // Skip the outer object
            if (content.contains("\"recentProjects\"") && pos == 0)
            {
                pos++;
                continue;
            }

            size_t end = content.find('}', pos);
            if (end == std::string::npos)
                break;

            std::string entry = content.substr(pos, end - pos + 1);
            std::string name = ExtractJsonString(entry, "name");
            std::string path = ExtractJsonString(entry, "path");
            std::string engineVer = ExtractJsonString(entry, "engineVersion");
            uint64_t lastOpened = ExtractJsonUint64(entry, "lastOpened");

            if (m_recentProjects.size() >= 15)
                break; // hard cap - the UI never shows more

            if (!path.empty())
            {
                RecentProject rp;
                rp.name = name;
                rp.path = NormalizeProjectPath(path);
                rp.engineVersion = engineVer;
                rp.lastOpened = lastOpened;
                std::error_code existsEc;
                rp.valid = fs::exists(PathFromUtf8(path), existsEc) && !existsEc;
                m_recentProjects.push_back(rp);
            }

            pos = end + 1;
        }

        std::cout << "Loaded " << m_recentProjects.size() << " recent projects\n";
    }

    void ProjectManager::SaveRecentProjectsList()
    {
        std::string filePath = GetRecentProjectsFilePath();

        try
        {
            const fs::path nativeFilePath = PathFromUtf8(filePath);
            fs::create_directories(nativeFilePath.parent_path());

            std::ofstream file(nativeFilePath);
            if (!file.is_open())
                return;

            file << "{\n";
            file << "  \"recentProjects\": [\n";
            const size_t count = std::min<size_t>(m_recentProjects.size(), 15);
            for (size_t i = 0; i < count; ++i)
            {
                const auto& rp = m_recentProjects[i];
                file << "    {\n";
                file << "      \"name\": \"" << EscapeJsonString(rp.name) << "\",\n";
                file << "      \"path\": \"" << EscapeJsonString(rp.path) << "\",\n";
                file << "      \"engineVersion\": \"" << EscapeJsonString(rp.engineVersion) << "\",\n";
                file << "      \"lastOpened\": " << rp.lastOpened << "\n";
                file << "    }";
                if (i + 1 < count)
                    file << ",";
                file << "\n";
            }
            file << "  ]\n";
            file << "}\n";
            file.close();

            if (m_fileCache)
            {
                m_fileCache->Invalidate(filePath);
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error saving recent projects: " << e.what() << "\n";
        }
    }

    // ------------------------------------------------------------------
    // Static template helpers
    // ------------------------------------------------------------------
    std::span<const ProjectTemplateDescriptor> ProjectManager::GetProjectTemplateDescriptors() noexcept
    {
        return kProjectTemplates;
    }

    const ProjectTemplateDescriptor* ProjectManager::FindProjectTemplateDescriptor(ProjectTemplate t) noexcept
    {
        const auto found =
            std::find_if(kProjectTemplates.begin(), kProjectTemplates.end(),
                         [t](const ProjectTemplateDescriptor& descriptor) { return descriptor.type == t; });
        return found == kProjectTemplates.end() ? nullptr : &*found;
    }

    const ProjectTemplateDescriptor* ProjectManager::FindProjectTemplateDescriptor(std::string_view identity) noexcept
    {
        const auto found = std::find_if(
            kProjectTemplates.begin(), kProjectTemplates.end(), [identity](const ProjectTemplateDescriptor& descriptor)
            { return descriptor.stableId == identity || descriptor.packageDirectory == identity; });
        return found == kProjectTemplates.end() ? nullptr : &*found;
    }

    std::string ProjectManager::GetProjectTemplateName(ProjectTemplate t)
    {
        const auto* descriptor = FindProjectTemplateDescriptor(t);
        return descriptor ? std::string(descriptor->displayName) : "Unknown";
    }

    std::string ProjectManager::GetProjectTemplateDescription(ProjectTemplate t)
    {
        const auto* descriptor = FindProjectTemplateDescriptor(t);
        return descriptor ? std::string(descriptor->description) : "";
    }

} // namespace SparkEditor
