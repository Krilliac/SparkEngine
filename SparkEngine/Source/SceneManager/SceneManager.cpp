#include "SceneManager.h"
#include "../Core/Platform.h"
#include "Game/GameObject.h"
#include "Game/CubeObject.h"
#include "Game/PlaneObject.h"
#include "Game/SphereObject.h"
#include "Game/PlaceholderMesh.h"
#include "Game/PyramidObject.h"
#include "Game/RampObject.h"
#include "Game/WallObject.h"
#include "Graphics/GraphicsEngine.h"
#include "Engine/Events/EventSystem.h"
#include "Core/EngineContext.h"
#include "Utils/Assert.h"
#include "Utils/DebugHookManager.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/ConsoleProcessManager.h"
#include "Utils/LocalFileCache.h"
#include "Utils/FileUtils.h"
#include "SceneManager/SceneTextFormat.h"

#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <optional>
#include <memory>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Helper: convert wstring path to string for cross-platform file I/O
static std::string WideToNarrow(const std::wstring& wide)
{
    std::string narrow;
    narrow.reserve(wide.size());
    for (wchar_t wc : wide)
        narrow.push_back(static_cast<char>(wc));
    return narrow;
}

// Narrow cache/file APIs are usable only when their spelling resolves to the
// same native path. Keep the existing fast path for ASCII/ACP-compatible
// paths, but never let a lossy conversion choose a different file or cache key.
static std::optional<std::string> NarrowPathIfRoundTrips(const std::wstring& wide)
{
    const std::string narrow = WideToNarrow(wide);
    try
    {
        if (std::filesystem::path(narrow) == std::filesystem::path(wide))
            return narrow;
    }
    catch (const std::exception&)
    {
    }
    return std::nullopt;
}

namespace
{
    bool FlushFileDurably(const std::filesystem::path& path, std::error_code& error)
    {
#if defined(_WIN32)
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return false;
        }
        const bool flushed = ::FlushFileBuffers(file) != FALSE;
        const DWORD flushError = flushed ? ERROR_SUCCESS : ::GetLastError();
        ::CloseHandle(file);
        if (!flushed)
        {
            error = std::error_code(static_cast<int>(flushError), std::system_category());
            return false;
        }
        return true;
#else
        const int file = ::open(path.c_str(), O_RDONLY);
        if (file < 0)
        {
            error = std::error_code(errno, std::generic_category());
            return false;
        }
        const bool flushed = ::fsync(file) == 0;
        const int flushError = flushed ? 0 : errno;
        ::close(file);
        if (!flushed)
        {
            error = std::error_code(flushError, std::generic_category());
            return false;
        }
        return true;
#endif
    }

    bool ReplaceFileAtomically(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                               std::error_code& error)
    {
#if defined(_WIN32)
        if (::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return false;
#else
        std::filesystem::rename(temporary, destination, error);
        if (error)
            return false;
        const auto directory = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
#if defined(O_DIRECTORY)
        const int directoryFile = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
        const int directoryFile = ::open(directory.c_str(), O_RDONLY);
#endif
        if (directoryFile < 0)
        {
            error = std::error_code(errno, std::generic_category());
            SPARK_LOG_WARN(Spark::LogCategory::Scene,
                           "SceneManager: destination replaced but directory fsync is unavailable: %s",
                           error.message().c_str());
            return true;
        }
        const bool flushed = ::fsync(directoryFile) == 0;
        const int flushError = flushed ? 0 : errno;
        ::close(directoryFile);
        if (!flushed)
        {
            error = std::error_code(flushError, std::generic_category());
            // rename() already made the new image visible. Do not report a
            // failed save or attempt rollback after that point: callers rely
            // on false meaning that the old destination remains intact, and
            // directory fsync is unsupported on some macOS filesystems.
            SPARK_LOG_WARN(Spark::LogCategory::Scene,
                           "SceneManager: destination replaced but directory fsync failed: %s",
                           error.message().c_str());
        }
        return true;
#endif
    }

    std::filesystem::path MakeUniqueTemporaryPath(const std::filesystem::path& destination)
    {
        static std::atomic<uint64_t> sequence{0};
        const auto threadHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed);
            std::filesystem::path candidate = destination;
            candidate += ".tmp." + std::to_string(threadHash) + "." + std::to_string(serial);
            std::error_code existsError;
            if (!std::filesystem::exists(candidate, existsError) && !existsError)
                return candidate;
        }
        return {};
    }

    bool WriteDurableText(const std::filesystem::path& path, const std::string& text, std::error_code& error)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
            return false;
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        output.close();
        if (output.fail())
            return false;
        return FlushFileDurably(path, error);
    }

    void RemoveFileNoThrow(const std::filesystem::path& path)
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    // ------------------------------------------------------------------
    // Root-confined save (SaveSceneWithinRoot)
    //
    // The destination is never re-resolved by path after it is validated.
    // Each directory below the trusted root is opened one component at a
    // time and kept open; the temporary file is created exclusively inside
    // that held directory and renamed over the destination there. POSIX
    // pins the directory with an fd (openat + O_NOFOLLOW); Windows pins
    // every component with a handle that denies FILE_SHARE_DELETE, so no
    // component can be renamed or replaced until the save finishes.
    // ------------------------------------------------------------------

    bool IsConfinedRelativeScenePath(const std::filesystem::path& relative)
    {
        if (relative.empty() || relative.has_root_name() || relative.has_root_directory())
            return false;
        for (const auto& component : relative)
        {
            if (component.empty() || component == "." || component == "..")
                return false;
        }
        const auto extension = relative.extension();
        return (extension == ".scene" || extension == ".json") && !relative.stem().empty();
    }

    std::filesystem::path MakeTemporaryName(const std::filesystem::path& leaf, uint64_t serial)
    {
        std::filesystem::path name = leaf;
        name += ".tmp." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "." +
                std::to_string(serial);
        return name;
    }

#if defined(_WIN32)
    struct HandleCloser
    {
        void operator()(HANDLE handle) const
        {
            if (handle && handle != INVALID_HANDLE_VALUE)
                ::CloseHandle(handle);
        }
    };
    using UniqueHandle = std::unique_ptr<void, HandleCloser>;

    // Opens a directory without following a reparse point in its last
    // component and without FILE_SHARE_DELETE, which pins it: while the
    // handle is open the directory cannot be renamed, deleted or replaced.
    UniqueHandle OpenPinnedDirectory(const std::filesystem::path& path, bool allowReparse)
    {
        const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (allowReparse ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
        UniqueHandle handle(::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE)
            return UniqueHandle(nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(handle.get(), &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (!allowReparse && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)))
            return UniqueHandle(nullptr);
        return handle;
    }

    std::wstring FinalPath(HANDLE handle)
    {
        std::wstring buffer(512, L'\0');
        for (;;)
        {
            const DWORD length = ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                             FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
            if (length == 0)
                return {};
            if (length < buffer.size())
            {
                buffer.resize(length);
                return buffer;
            }
            buffer.resize(length + 1);
        }
    }

    bool EqualsIgnoringCase(const std::wstring& left, const std::wstring& right)
    {
        return ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                      static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
    }

    bool SaveTextWithinRoot(const std::filesystem::path& root, const std::filesystem::path& relative,
                            const std::string& text)
    {
        // The root itself is trusted configuration; it is pinned but may be
        // reached through a link. Everything below it must be a plain directory.
        std::vector<UniqueHandle> pins;
        pins.push_back(OpenPinnedDirectory(root, /*allowReparse=*/true));
        if (!pins.back())
            return false;
        const std::wstring rootFinal = FinalPath(pins.back().get());

        std::filesystem::path directory = root;
        const std::filesystem::path leaf = relative.filename();
        for (const auto& component : relative.parent_path())
        {
            directory /= component;
            pins.push_back(OpenPinnedDirectory(directory, /*allowReparse=*/false));
            if (!pins.back())
                return false;
        }
        const std::wstring directoryFinal = FinalPath(pins.back().get());
        if (rootFinal.empty() || directoryFinal.empty() ||
            (directoryFinal != rootFinal && directoryFinal.rfind(rootFinal + L"\\", 0) != 0))
            return false;

        // An existing destination must be a plain file, not a link or directory.
        const std::filesystem::path destination = directory / leaf;
        const DWORD existing = ::GetFileAttributesW(destination.c_str());
        if (existing != INVALID_FILE_ATTRIBUTES &&
            (existing & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            return false;

        static std::atomic<uint64_t> sequence{0};
        UniqueHandle file(nullptr);
        for (int attempt = 0; attempt < 64 && !file; ++attempt)
        {
            const auto temporary = directory / MakeTemporaryName(leaf, sequence.fetch_add(1));
            HANDLE created = ::CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (created != INVALID_HANDLE_VALUE)
                file.reset(created);
            else if (::GetLastError() != ERROR_FILE_EXISTS)
                return false;
        }
        if (!file)
            return false;

        const auto discard = [&file]
        {
            FILE_DISPOSITION_INFO disposition{TRUE};
            ::SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition));
        };

        size_t written = 0;
        while (written < text.size())
        {
            DWORD chunk = 0;
            const DWORD request = static_cast<DWORD>(std::min<size_t>(text.size() - written, 1u << 20));
            if (!::WriteFile(file.get(), text.data() + written, request, &chunk, nullptr) || chunk == 0)
            {
                discard();
                return false;
            }
            written += chunk;
        }
        if (!::FlushFileBuffers(file.get()))
        {
            discard();
            return false;
        }

        // Rename by handle into the pinned directory, replacing the old file.
        const std::wstring target = destination.wstring();
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
        auto* renameInfo = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        renameInfo->ReplaceIfExists = TRUE;
        renameInfo->RootDirectory = nullptr;
        renameInfo->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
        std::memcpy(renameInfo->FileName, target.data(), target.size() * sizeof(wchar_t));
        if (!::SetFileInformationByHandle(file.get(), FileRenameInfo, renameInfo, static_cast<DWORD>(buffer.size())))
        {
            discard();
            return false;
        }
        ::FlushFileBuffers(file.get());

        // Re-verify where the bytes actually live now.
        const std::wstring fileFinal = FinalPath(file.get());
        const size_t split = fileFinal.find_last_of(L'\\');
        if (split == std::wstring::npos || fileFinal.substr(0, split) != directoryFinal ||
            !EqualsIgnoringCase(fileFinal.substr(split + 1), leaf.wstring()))
        {
            discard();
            return false;
        }
        return true;
    }
#else
    struct FdCloser
    {
        int fd = -1;
        ~FdCloser()
        {
            if (fd >= 0)
                ::close(fd);
        }
    };

    // Walks `relative`'s directories from an open root fd, refusing any
    // symlink component. Returns -1 on failure; the caller owns the fd.
    int OpenDirectoryBelow(int rootFd, const std::filesystem::path& relativeDirectory)
    {
        int current = ::dup(rootFd);
        for (const auto& component : relativeDirectory)
        {
            if (current < 0)
                return -1;
            const int next = ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            ::close(current);
            current = next;
        }
        return current;
    }

    bool SameInode(int left, int right)
    {
        struct stat a = {};
        struct stat b = {};
        return ::fstat(left, &a) == 0 && ::fstat(right, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    }

    bool SaveTextWithinRoot(const std::filesystem::path& root, const std::filesystem::path& relative,
                            const std::string& text)
    {
        // The root itself is trusted configuration; everything below it is
        // reached only through held directory fds with O_NOFOLLOW.
        FdCloser rootFd{::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (rootFd.fd < 0)
            return false;
        FdCloser directory{OpenDirectoryBelow(rootFd.fd, relative.parent_path())};
        if (directory.fd < 0)
            return false;

        // An existing destination must be a plain file, not a link or directory.
        const std::string leaf = relative.filename().string();
        struct stat existing = {};
        if (::fstatat(directory.fd, leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(existing.st_mode))
            return false;

        static std::atomic<uint64_t> sequence{0};
        std::string temporary;
        FdCloser file;
        for (int attempt = 0; attempt < 64 && file.fd < 0; ++attempt)
        {
            temporary = MakeTemporaryName(leaf, sequence.fetch_add(1)).string();
            file.fd =
                ::openat(directory.fd, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
            if (file.fd < 0 && errno != EEXIST)
                return false;
        }
        if (file.fd < 0)
            return false;

        size_t written = 0;
        while (written < text.size())
        {
            const ssize_t chunk = ::write(file.fd, text.data() + written, text.size() - written);
            if (chunk < 0 && errno == EINTR)
                continue;
            if (chunk <= 0)
            {
                ::unlinkat(directory.fd, temporary.c_str(), 0);
                return false;
            }
            written += static_cast<size_t>(chunk);
        }
        if (::fsync(file.fd) != 0 || ::renameat(directory.fd, temporary.c_str(), directory.fd, leaf.c_str()) != 0)
        {
            ::unlinkat(directory.fd, temporary.c_str(), 0);
            return false;
        }
        ::fsync(directory.fd); // best effort, as in ReplaceFileAtomically

        // Re-verify after the rename: the name now refers to our file, and
        // the held directory is still the one reached from the root.
        struct stat placed = {};
        struct stat ours = {};
        FdCloser recheck{OpenDirectoryBelow(rootFd.fd, relative.parent_path())};
        const bool inPlace = ::fstatat(directory.fd, leaf.c_str(), &placed, AT_SYMLINK_NOFOLLOW) == 0 &&
                             ::fstat(file.fd, &ours) == 0 && placed.st_dev == ours.st_dev &&
                             placed.st_ino == ours.st_ino;
        if (!inPlace || recheck.fd < 0 || !SameInode(recheck.fd, directory.fd))
        {
            if (inPlace)
                ::unlinkat(directory.fd, leaf.c_str(), 0);
            return false;
        }
        return true;
    }
#endif
} // namespace

// Use logging macros from LogMacros.h (included transitively via headers)

SceneManager::SceneManager(GraphicsEngine* graphics, InputManager* input) : m_graphics(graphics), m_input(input)
{
    // Both pointers are optional. Every GameObject instantiation path checks
    // m_graphics, so null graphics selects data-only loading (the headless FPS
    // arena); m_input is only stored for callers and never dereferenced here.
    SPARK_LOG_INFO(Spark::LogCategory::Scene, "SceneManager constructed (%s)",
                   graphics ? "graphics attached" : "data-only, no graphics");
}

SceneManager::~SceneManager()
{
    // Join any in-flight async load thread to avoid use-after-free
    if (m_asyncLoadThread.joinable())
    {
        m_asyncLoadThread.join();
    }
}

const std::vector<std::unique_ptr<GameObject>>& SceneManager::GetObjects() const
{
    return m_objects;
}

// ============================================================================
// Scene Loading / Saving
// ============================================================================

bool SceneManager::LoadScene(const std::wstring& filepath)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !filepath.empty(),
                      "SceneManager::LoadScene — filepath must not be empty");
    std::string narrowName(filepath.begin(), filepath.end());
    SPARK_DEBUG_HOOK_SCENE(ScenePreLoad, narrowName);

    // Inspect native path components before any file access. Narrowing a wide
    // profile path can change its spelling and is not a traversal check.
    const std::filesystem::path nativeScenePath(filepath);
    if (std::find(nativeScenePath.begin(), nativeScenePath.end(), std::filesystem::path(L"..")) !=
        nativeScenePath.end())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Path traversal rejected: " + filepath, L"ERROR");
        return false;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager::LoadScene called. filepath=" + filepath, L"OPERATION");

    // Loading formats currently clear the live graph before parsing. Keep a
    // value snapshot so a malformed or unreadable replacement cannot leave the
    // running editor/game with an empty half-scene. Objects are recreated from
    // the restored nodes below; this is intentionally synchronous and bounded
    // to the existing scene-load path.
    const auto previousNodes = m_sceneNodes;
    const auto previousMetadata = m_metadata;
    const auto previousFilePath = m_currentFilePath;
    const bool previousDirty = m_dirty;
    // Keep ownership of the live objects out of the loader while it stages a
    // replacement.  Re-instantiating on rollback is observably wrong: callers
    // may retain a GameObject pointer and runtime state (health, physics,
    // component state, etc.) must survive a failed reload byte-for-byte.
    auto previousObjects = std::move(m_objects);
    const auto restorePrevious =
        [this, &previousNodes, &previousMetadata, &previousFilePath, previousDirty, &previousObjects]()
    {
        m_objects.clear();
        m_sceneNodes = previousNodes;
        m_metadata = previousMetadata;
        m_currentFilePath = previousFilePath;
        m_dirty = previousDirty;
        m_nodeNameIndex.clear();
        for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
        {
            if (!m_sceneNodes[i].name.empty())
                m_nodeNameIndex[m_sceneNodes[i].name] = i;
        }
        m_objects = std::move(previousObjects);
    };

    auto ext = std::filesystem::path(filepath).extension();
    bool loaded = false;
    const bool previousEventSuppression = m_suppressSceneEvents;
    m_suppressSceneEvents = true;

    try
    {
        if (ext == L".sparkscene")
            loaded = LoadReflected(filepath);
        else if (ext == L".scene")
            loaded = LoadCustom(filepath);
        else if (ext == L".json")
            loaded = LoadJSON(filepath);
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Scene file extension not recognized: " + filepath, L"WARNING");
            restorePrevious();
            m_suppressSceneEvents = previousEventSuppression;
            return false;
        }
    }
    catch (...)
    {
        restorePrevious();
        m_suppressSceneEvents = previousEventSuppression;
        return false;
    }

    if (!loaded)
    {
        restorePrevious();
    }
    else
        // The replacement is now committed; release the old graph only after
        // all parsing and instantiation succeeded.
        previousObjects.clear();

    m_suppressSceneEvents = previousEventSuppression;

    if (loaded)
    {
        m_currentFilePath = filepath;
        m_dirty = false;
        const auto instantiated =
            std::count_if(m_objects.begin(), m_objects.end(), [](const auto& object) { return object != nullptr; });
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene loaded: " + std::to_wstring(instantiated) + L" objects, " +
                                     std::to_wstring(m_sceneNodes.size()) + L" nodes",
                                 L"SUCCESS");

        SPARK_DEBUG_HOOK_SCENE(ScenePostLoad, narrowName);

        // Publish SceneLoadedEvent
        if (auto* ctx = EngineContext::Get())
        {
            if (auto* bus = ctx->GetEventBus())
            {
                std::string narrowPath(filepath.begin(), filepath.end());
                bus->Publish(Spark::SceneLoadedEvent{narrowPath});
            }
        }
    }
    return loaded;
}

bool SceneManager::SaveScene(const std::wstring& filepath) const
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !filepath.empty(),
                      "SceneManager::SaveScene — filepath must not be empty");
    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager::SaveScene called. filepath=" + filepath, L"OPERATION");
    bool saved = SaveJSON(filepath);
    if (saved)
    {
        m_currentFilePath = filepath;
        m_dirty = false;
    }
    return saved;
}

bool SceneManager::SaveSceneWithinRoot(const std::filesystem::path& root,
                                       const std::filesystem::path& relativePath) const
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
    const std::filesystem::path destination = root / relativePath;
    if (root.empty() || !IsConfinedRelativeScenePath(relativePath))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Scene save path rejected: " + relativePath.wstring(), L"ERROR");
        return false;
    }

    std::string text;
    if (!SerializeSceneText(text, destination.wstring()))
        return false;

    bool saved = false;
    try
    {
        saved = SaveTextWithinRoot(root, relativePath, text);
    }
    catch (const std::exception&)
    {
        saved = false;
    }
    if (!saved)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Scene save refused or failed (missing directory, link component, "
                                 L"or I/O error): " +
                                     destination.wstring(),
                                 L"ERROR");
        return false;
    }

    if (m_fileCache)
    {
        if (const auto narrowPath = NarrowPathIfRoundTrips(destination.wstring()))
            m_fileCache->Invalidate(*narrowPath);
    }
    m_currentFilePath = destination.wstring();
    m_dirty = false;
    LOG_TO_CONSOLE_IMMEDIATE(L"Scene saved: " + std::to_wstring(m_sceneNodes.size()) + L" nodes", L"SUCCESS");
    return true;
}

void SceneManager::LoadSceneAsync(const std::wstring& filepath, SceneLoadCallback callback)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !filepath.empty(),
                      "SceneManager::LoadSceneAsync — filepath must not be empty");
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Scene, callback);
    // Join any previous async load before starting a new one
    if (m_asyncLoadThread.joinable())
    {
        m_asyncLoadThread.join();
    }

    m_asyncLoading = true;
    m_asyncLoadThread = std::thread(
        [this, filepath, callback]()
        {
            bool success = false;
            {
                std::lock_guard<std::mutex> lock(m_sceneMutex);
                success = LoadScene(filepath);
            }
            m_asyncLoading = false;
            if (callback)
                callback(success, std::string(filepath.begin(), filepath.end()));
        });
}

// ============================================================================
// Scene Hierarchy
// ============================================================================

int SceneManager::AddNode(const SceneNode& node)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !node.name.empty(),
                      "SceneManager::AddNode — node name must not be empty");
    int index = static_cast<int>(m_sceneNodes.size());
    m_sceneNodes.push_back(node);

    // Update name-to-index cache
    if (!node.name.empty())
    {
        m_nodeNameIndex[node.name] = index;
    }

    // Update parent's child list if parent specified
    if (node.parentIndex >= 0 && node.parentIndex < static_cast<int>(m_sceneNodes.size()))
    {
        m_sceneNodes[node.parentIndex].childIndices.push_back(index);
    }

    m_dirty = true;
    return index;
}

void SceneManager::RemoveNode(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sceneNodes.size()))
        return;

    // Recursively remove children first
    auto& node = m_sceneNodes[index];
    for (int childIdx : node.childIndices)
        RemoveNode(childIdx);

    // Remove from parent's child list
    if (node.parentIndex >= 0 && node.parentIndex < static_cast<int>(m_sceneNodes.size()))
    {
        auto& parentChildren = m_sceneNodes[node.parentIndex].childIndices;
        parentChildren.erase(std::remove(parentChildren.begin(), parentChildren.end(), index), parentChildren.end());
    }

    // Remove from name cache before tombstoning
    if (!node.name.empty())
    {
        m_nodeNameIndex.erase(node.name);
    }

    // Mark as removed (tombstone approach to avoid index invalidation)
    node.name = "";
    node.type = "";
    m_dirty = true;
}

void SceneManager::SetParent(int childIndex, int parentIndex)
{
    if (childIndex < 0 || childIndex >= static_cast<int>(m_sceneNodes.size()))
        return;

    auto& child = m_sceneNodes[childIndex];

    // Remove from old parent
    if (child.parentIndex >= 0 && child.parentIndex < static_cast<int>(m_sceneNodes.size()))
    {
        auto& oldParentChildren = m_sceneNodes[child.parentIndex].childIndices;
        oldParentChildren.erase(std::remove(oldParentChildren.begin(), oldParentChildren.end(), childIndex),
                                oldParentChildren.end());
    }

    // Set new parent
    child.parentIndex = parentIndex;
    if (parentIndex >= 0 && parentIndex < static_cast<int>(m_sceneNodes.size()))
    {
        m_sceneNodes[parentIndex].childIndices.push_back(childIndex);
    }

    m_dirty = true;
}

std::vector<int> SceneManager::GetRootNodes() const
{
    std::vector<int> roots;
    for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
    {
        if (m_sceneNodes[i].parentIndex < 0 && !m_sceneNodes[i].type.empty())
            roots.push_back(i);
    }
    return roots;
}

const SceneNode* SceneManager::GetNode(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_sceneNodes.size()))
        return nullptr;
    return &m_sceneNodes[index];
}

SceneNode* SceneManager::GetNode(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sceneNodes.size()))
        return nullptr;
    return &m_sceneNodes[index];
}

int SceneManager::FindNode(const std::string& name) const
{
    // O(1) lookup via name cache instead of O(n) linear scan
    auto it = m_nodeNameIndex.find(name);
    if (it != m_nodeNameIndex.end())
        return it->second;
    return -1;
}

// ============================================================================
// Scene State
// ============================================================================

void SceneManager::NewScene(const std::string& name)
{
    Clear();
    m_metadata.sceneName = name;
    m_currentFilePath.clear();
    m_dirty = false;
}

void SceneManager::Clear()
{
    // Publish SceneUnloadedEvent before clearing
    if (!m_suppressSceneEvents && !m_currentFilePath.empty())
    {
        if (auto* ctx = EngineContext::Get())
        {
            if (auto* bus = ctx->GetEventBus())
            {
                std::string narrowPath(m_currentFilePath.begin(), m_currentFilePath.end());
                bus->Publish(Spark::SceneUnloadedEvent{narrowPath});
            }
        }
    }

    m_objects.clear();
    m_sceneNodes.clear();
    m_nodeNameIndex.clear();
    m_dirty = true;
}

std::vector<std::string> SceneManager::GetAvailableScenes(const std::wstring& directory) const
{
    std::vector<std::string> scenes;
    if (!std::filesystem::exists(directory))
        return scenes;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        auto ext = entry.path().extension();
        if (ext == L".scene" || ext == L".json")
        {
            // UTF-8, never the ANSI code page: path::string() throws on Windows for a
            // scene name that code page cannot spell, which ended the whole listing.
            if (auto name = Spark::FileUtils::TryPathToUtf8(entry.path().filename()))
                scenes.push_back(std::move(*name));
        }
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

// ============================================================================
// JSON Scene Format
// ============================================================================

bool SceneManager::LoadJSON(const std::wstring& path)
{
    std::string content;
    const std::filesystem::path nativePath(path);

    if (m_fileCache)
    {
        if (const auto narrowPath = NarrowPathIfRoundTrips(path))
        {
            auto result = m_fileCache->ReadText(*narrowPath);
            if (result.IsOk())
                content = result.Value();
        }
    }

    if (content.empty())
    {
        std::ifstream file(nativePath);
        if (!file.is_open())
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot open JSON scene: " + path, L"ERROR");
            return false;
        }
        content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
    }

    // The versioned text format (not JSON despite the method name) is parsed
    // into isolated state: a malformed row must not clear or partially replace
    // the live scene (LoadScene also preserves the object graph).
    SceneMetadata stagedMetadata = m_metadata;
    std::vector<SceneNode> stagedNodes;
    if (!Spark::ParseVersionedSceneText(content, stagedMetadata, stagedNodes))
        return false;

    Clear();
    m_metadata = std::move(stagedMetadata);
    m_sceneNodes = std::move(stagedNodes);
    m_nodeNameIndex.clear();
    for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
        m_nodeNameIndex[m_sceneNodes[static_cast<size_t>(i)].name] = i;
    InstantiateNodes();
    return true;
}

bool SceneManager::SerializeSceneText(std::string& text, const std::wstring& pathForLog) const
{
    if (!Spark::SerializeVersionedSceneText(m_metadata, m_sceneNodes, text))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot save malformed scene hierarchy: " + pathForLog, L"ERROR");
        return false;
    }
    return true;
}

bool SceneManager::SaveJSON(const std::wstring& path) const
{
    std::string text;
    if (!SerializeSceneText(text, path))
        return false;

    const std::filesystem::path destination(path);
    const std::filesystem::path temporary = MakeUniqueTemporaryPath(destination);
    if (temporary.empty())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot allocate unique temporary scene path: " + path, L"ERROR");
        return false;
    }
    std::error_code error;
    if (!WriteDurableText(temporary, text, error) || !ReplaceFileAtomically(temporary, destination, error))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Cannot save scene atomically: " + path, L"ERROR");
        RemoveFileNoThrow(temporary);
        return false;
    }

    if (m_fileCache)
    {
        if (const auto narrowPath = NarrowPathIfRoundTrips(path))
            m_fileCache->Invalidate(*narrowPath);
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Scene saved: " + std::to_wstring(m_sceneNodes.size()) + L" nodes", L"SUCCESS");
    return true;
}

void SceneManager::InstantiateNodes()
{
    if (!m_graphics || !m_graphics->GetDevice() || !m_graphics->GetContext())
    {
        // Data-only scenes still preserve the documented node/object index
        // relationship; mesh slots remain null until a graphics device exists.
        m_objects.clear();
        m_objects.resize(m_sceneNodes.size());
        return;
    }

    for (const auto& node : m_sceneNodes)
    {
        if (node.type.empty())
        {
            m_objects.push_back(nullptr);
            continue;
        }

        // These authored scene entries carry game data, not renderable meshes.
        if (node.type == "Camera" || node.type == "SpawnPoint")
        {
            m_objects.push_back(nullptr);
            continue;
        }

        std::unique_ptr<GameObject> obj;

        // Determine mesh path: use modelPath if specified, otherwise construct from type
        std::wstring meshPath;
        if (!node.modelPath.empty())
        {
            meshPath = std::wstring(node.modelPath.begin(), node.modelPath.end());
        }

        // Primitives are built at UNIT size and scaled via the node's scale so
        // non-uniform scales work; their constructor geometry is authoritative
        // (no mesh file load). Previously the ctor took node.scale.x AND
        // LoadOrPlaceholderMesh replaced the geometry with a 1 m placeholder
        // cube whenever "Assets/Models/<type>.obj" did not exist — a 4096 m
        // scene terrain plane silently rendered as a 1 m cube.
        bool isPrimitive = true;
        if (node.type == "Cube" || node.type == "cube")
            obj = std::make_unique<CubeObject>(1.0f);
        else if (node.type == "Plane" || node.type == "plane")
            obj = std::make_unique<PlaneObject>(1.0f, 1.0f);
        else if (node.type == "Sphere" || node.type == "sphere")
            obj = std::make_unique<SphereObject>(0.5f, 16, 16);
        else if (node.type == "Pyramid" || node.type == "pyramid")
            obj = std::make_unique<PyramidObject>(1.0f);
        else if (node.type == "Ramp" || node.type == "ramp")
            obj = std::make_unique<RampObject>(1.0f, 1.0f);
        else if (node.type == "Wall" || node.type == "wall")
            obj = std::make_unique<WallObject>(1.0f, 1.0f);
        else if (node.type == "model" || node.type == "Model")
        {
            // Model type: create a cube as placeholder geometry, then load the actual mesh
            isPrimitive = false;
            obj = std::make_unique<CubeObject>(1.0f);
            if (meshPath.empty())
            {
                // No modelPath specified, skip this model node
                LOG_TO_CONSOLE(L"SceneManager: model node '" + std::wstring(node.name.begin(), node.name.end()) +
                                   L"' has no modelPath, using placeholder",
                               L"WARNING");
            }
        }
        else
        {
            LOG_TO_CONSOLE(L"SceneManager: Unknown node type '" + std::wstring(node.type.begin(), node.type.end()) +
                               L"', skipping",
                           L"WARNING");
            m_objects.push_back(nullptr);
            continue;
        }

        if (obj)
        {
            HRESULT hr = obj->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (SUCCEEDED(hr))
            {
                if (!isPrimitive)
                    LoadOrPlaceholderMesh(*obj->GetMesh(), m_graphics->GetDevice(), m_graphics->GetContext(), meshPath);
                obj->SetPosition(node.position);
                // SceneNode rotations are authored in degrees; GameObject
                // stores radians (XMMatrixRotationRollPitchYaw input).
                constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
                obj->SetRotation(
                    {node.rotation.x * kDegToRad, node.rotation.y * kDegToRad, node.rotation.z * kDegToRad});
                obj->SetScale(node.scale);
                obj->SetName(node.name);
                obj->SetMaterialPath(node.materialPath);
                m_objects.push_back(std::move(obj));
            }
            else
            {
                m_objects.push_back(nullptr);
            }
        }
    }
}

// ============================================================================
// Legacy .scene loader (preserved for backward compatibility)
// ============================================================================

bool SceneManager::LoadCustom(const std::wstring& path)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager::LoadCustom called. path=" + path, L"OPERATION");

    std::ifstream file{std::filesystem::path(path)};
    if (!file.is_open())
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Could not open scene file: " + path, L"ERROR");
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    switch (Spark::DetectSceneTextDialect(content))
    {
    case Spark::SceneTextDialect::Versioned:
        return LoadJSON(path);

    case Spark::SceneTextDialect::Ini:
    {
        SceneMetadata metadata;
        std::vector<SceneNode> nodes;
        if (!Spark::ParseIniSceneText(content, metadata, nodes))
            return false;
        Clear();
        m_metadata = std::move(metadata);
        m_sceneNodes = std::move(nodes);
        m_nodeNameIndex.clear();
        for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
            m_nodeNameIndex.emplace(m_sceneNodes[static_cast<size_t>(i)].name, i);
        InstantiateNodes();
        break;
    }

    case Spark::SceneTextDialect::LegacyObjects:
    {
        // Legacy space-delimited format: Type X Y Z [params...]
        if (!m_graphics || !m_graphics->GetDevice() || !m_graphics->GetContext())
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Legacy object format requires a graphics device/context",
                                     L"ERROR");
            return false;
        }
        std::vector<Spark::LegacyObjectRow> rows;
        if (!Spark::ParseLegacyObjectLines(content, rows))
            return false;

        std::vector<SceneNode> stagedNodes;
        std::vector<std::unique_ptr<GameObject>> stagedObjects;
        for (const Spark::LegacyObjectRow& row : rows)
        {
            // The parser guarantees every dimension and tessellation value the
            // primitive constructors require.
            std::unique_ptr<GameObject> obj;
            if (row.type == "Cube")
                obj = std::make_unique<CubeObject>(row.primary);
            else if (row.type == "Plane")
                obj = std::make_unique<PlaneObject>(row.primary, row.secondary);
            else if (row.type == "Sphere")
                obj = std::make_unique<SphereObject>(row.primary, row.slices, row.stacks);
            else if (row.type == "Pyramid")
                obj = std::make_unique<PyramidObject>(row.primary);
            else if (row.type == "Ramp")
                obj = std::make_unique<RampObject>(row.primary, row.secondary);
            else
                obj = std::make_unique<WallObject>(row.primary, row.secondary);

            HRESULT hr = obj->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (FAILED(hr))
            {
                LOG_TO_CONSOLE(L"SceneManager: object Initialize failed on line " + std::to_wstring(row.lineNumber),
                               L"ERROR");
                return false;
            }

            std::string lowerType = row.type;
            std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), ::tolower);
            std::wstring meshPath = L"Assets\\Models\\" + std::wstring(lowerType.begin(), lowerType.end()) + L".obj";
            LoadOrPlaceholderMesh(*obj->GetMesh(), m_graphics->GetDevice(), m_graphics->GetContext(), meshPath);
            obj->SetPosition(row.position);
            SceneNode node;
            node.type = row.type;
            node.name = row.type + "_" + std::to_string(row.lineNumber);
            node.position = row.position;
            stagedNodes.push_back(std::move(node));
            stagedObjects.push_back(std::move(obj));
        }
        Clear();
        m_sceneNodes = std::move(stagedNodes);
        m_objects = std::move(stagedObjects);
        m_nodeNameIndex.clear();
        for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
            m_nodeNameIndex.emplace(m_sceneNodes[static_cast<size_t>(i)].name, i);
        break;
    }
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"SceneManager: Loaded " + std::to_wstring(m_sceneNodes.size()) + L" nodes", L"SUCCESS");
    return !m_sceneNodes.empty();
}

// ============================================================================
// Console Integration
// ============================================================================

std::string SceneManager::Console_ListNodes() const
{
    std::ostringstream ss;
    ss << "=== Scene: " << m_metadata.sceneName << " ===\n";
    ss << "Nodes: " << m_sceneNodes.size() << " | Objects: " << m_objects.size() << "\n";

    for (int i = 0; i < static_cast<int>(m_sceneNodes.size()); ++i)
    {
        const auto& node = m_sceneNodes[i];
        if (node.type.empty())
            continue;

        std::string indent(node.parentIndex >= 0 ? 4 : 2, ' ');
        ss << indent << "[" << i << "] " << node.name << " (" << node.type << ") " << "pos=(" << node.position.x << ","
           << node.position.y << "," << node.position.z << ")\n";
    }
    return ss.str();
}

std::string SceneManager::Console_GetNodeInfo(int index) const
{
    const auto* node = GetNode(index);
    if (!node)
        return "Node not found: " + std::to_string(index);

    std::ostringstream ss;
    ss << "=== Node [" << index << "] ===\n"
       << "  Name: " << node->name << "\n"
       << "  Type: " << node->type << "\n"
       << "  Position: (" << node->position.x << ", " << node->position.y << ", " << node->position.z << ")\n"
       << "  Rotation: (" << node->rotation.x << ", " << node->rotation.y << ", " << node->rotation.z << ")\n"
       << "  Scale: (" << node->scale.x << ", " << node->scale.y << ", " << node->scale.z << ")\n"
       << "  Parent: " << node->parentIndex << "\n"
       << "  Children: " << node->childIndices.size() << "\n";
    return ss.str();
}

bool SceneManager::Console_MoveNode(int index, float x, float y, float z)
{
    auto* node = GetNode(index);
    if (!node)
        return false;
    node->position = {x, y, z};
    m_dirty = true;
    return true;
}

bool SceneManager::Console_RenameNode(int index, const std::string& newName)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Scene, !newName.empty(),
                      "SceneManager::Console_RenameNode — newName must not be empty");
    auto* node = GetNode(index);
    if (!node)
        return false;

    // Reject if another node already has this name (per documentation contract)
    int existing = FindNode(newName);
    if (existing >= 0 && existing != index)
        return false;

    node->name = newName;
    m_dirty = true;
    return true;
}
