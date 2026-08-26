/**
 * @file DynamicPluginHost.cpp
 * @brief Cross-platform implementation of Spark's stable C plugin ABI host.
 */

#include "DynamicPluginHost.h"
#include "FileIntegrity.h"
#include "Utils/JsonUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark
{
    namespace
    {
        struct AllocationHeader
        {
            void* base = nullptr;
            uint64_t size = 0;
        };

        bool IsPowerOfTwo(uint64_t value)
        {
            return value != 0 && (value & (value - 1)) == 0;
        }

        void* HostAllocate(uint64_t size, uint64_t alignment)
        {
            if (size == 0)
                return nullptr;
            alignment = std::max<uint64_t>(alignment, alignof(void*));
            if (!IsPowerOfTwo(alignment) || alignment > (UINT64_C(1) << 20) ||
                size > std::numeric_limits<size_t>::max() - alignment - sizeof(AllocationHeader))
                return nullptr;

            const size_t bytes = static_cast<size_t>(size + alignment - 1 + sizeof(AllocationHeader));
            void* base = std::malloc(bytes);
            if (!base)
                return nullptr;
            const uintptr_t first = reinterpret_cast<uintptr_t>(base) + sizeof(AllocationHeader);
            const uintptr_t aligned = (first + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
            auto* header = reinterpret_cast<AllocationHeader*>(aligned) - 1;
            header->base = base;
            header->size = size;
            return reinterpret_cast<void*>(aligned);
        }

        void HostDeallocate(void* memory)
        {
            if (!memory)
                return;
            auto* header = reinterpret_cast<AllocationHeader*>(memory) - 1;
            std::free(header->base);
        }

        std::string ResultMessage(const char* action, SparkPluginResult result)
        {
            return std::string(action) + " failed with plugin result " + std::to_string(result);
        }

        constexpr uint64_t kMaximumReloadStateBytes = UINT64_C(16) * 1024u * 1024u;

        bool HasHotReloadContract(const SparkPluginDescriptor* descriptor)
        {
            if (!descriptor || !descriptor->api)
                return false;
            const auto* api = descriptor->api;
            return (api->capabilities & SPARK_PLUGIN_CAP_HOT_RELOAD) != 0 && api->abi_minor >= 1 &&
                   api->struct_size >= offsetof(SparkPluginAPI, reserved) && api->prepare_unload && api->save_state &&
                   api->restore_state && api->cancel_unload;
        }

        struct PluginMetadata
        {
            std::string id;
            std::string version;
            std::string expectedHash;
        };

        struct PluginFileIdentity
        {
#if defined(_WIN32)
            DWORD volumeSerial = 0;
            DWORD fileIndexHigh = 0;
            DWORD fileIndexLow = 0;
#else
            dev_t device = 0;
            ino_t inode = 0;
#endif
            uint64_t size = 0;
        };

#if defined(_WIN32)
        class StablePluginFile
        {
          public:
            ~StablePluginFile()
            {
                if (m_handle != INVALID_HANDLE_VALUE)
                    ::CloseHandle(m_handle);
            }

            StablePluginFile(const StablePluginFile&) = delete;
            StablePluginFile& operator=(const StablePluginFile&) = delete;
            StablePluginFile() = default;

            bool Open(const std::filesystem::path& path, std::string& error)
            {
                // Deny writers and delete/rename operations until the loader has
                // mapped and identified the exact file that was hashed.
                m_handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
                if (m_handle == INVALID_HANDLE_VALUE)
                {
                    error =
                        "failed to lock plugin binary for validation (error " + std::to_string(::GetLastError()) + ")";
                    return false;
                }
                return true;
            }

            [[nodiscard]] HANDLE Get() const noexcept { return m_handle; }

          private:
            HANDLE m_handle = INVALID_HANDLE_VALUE;
        };

        bool CapturePluginFileIdentity(HANDLE handle, PluginFileIdentity& identity, std::string& error)
        {
            BY_HANDLE_FILE_INFORMATION information{};
            if (!::GetFileInformationByHandle(handle, &information))
            {
                error = "failed to query plugin file identity (error " + std::to_string(::GetLastError()) + ")";
                return false;
            }
            if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                error = "plugin binary must not be a reparse point";
                return false;
            }
            identity.volumeSerial = information.dwVolumeSerialNumber;
            identity.fileIndexHigh = information.nFileIndexHigh;
            identity.fileIndexLow = information.nFileIndexLow;
            identity.size = (static_cast<uint64_t>(information.nFileSizeHigh) << 32u) | information.nFileSizeLow;
            return true;
        }

        bool CapturePluginFileIdentity(const std::filesystem::path& path, PluginFileIdentity& identity,
                                       std::string& error)
        {
            HANDLE handle = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                error = "failed to open mapped plugin for identity validation (error " +
                        std::to_string(::GetLastError()) + ")";
                return false;
            }
            const bool captured = CapturePluginFileIdentity(handle, identity, error);
            ::CloseHandle(handle);
            return captured;
        }
#else
        class PrivatePluginCopy
        {
          public:
            ~PrivatePluginCopy() { Cleanup(); }

            PrivatePluginCopy(const PrivatePluginCopy&) = delete;
            PrivatePluginCopy& operator=(const PrivatePluginCopy&) = delete;
            PrivatePluginCopy() = default;

            bool Create(const std::filesystem::path& source, std::string& error)
            {
                std::error_code ec;
                auto pattern = (std::filesystem::temp_directory_path(ec) / "spark-plugin-XXXXXX").string();
                if (ec)
                {
                    error = "failed to locate the temporary directory for private plugin staging";
                    return false;
                }
                std::vector<char> mutablePattern(pattern.begin(), pattern.end());
                mutablePattern.push_back('\0');
                char* created = ::mkdtemp(mutablePattern.data());
                if (!created)
                {
                    error = "failed to create a private plugin staging directory";
                    return false;
                }
                m_directory = created;
                m_path = m_directory / source.filename();

                const int input = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (input < 0)
                {
                    error = "failed to securely open the plugin binary for private staging";
                    Cleanup();
                    return false;
                }
                struct stat information = {};
                if (::fstat(input, &information) != 0 || !S_ISREG(information.st_mode))
                {
                    ::close(input);
                    error = "plugin binary must be a regular non-symlink file";
                    Cleanup();
                    return false;
                }
                const int output = ::open(m_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0500);
                if (output < 0)
                {
                    ::close(input);
                    error = "failed to create the private staged plugin binary";
                    Cleanup();
                    return false;
                }

                bool copied = true;
                std::array<uint8_t, 64 * 1024> buffer{};
                for (;;)
                {
                    ssize_t count = ::read(input, buffer.data(), buffer.size());
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count < 0)
                    {
                        copied = false;
                        break;
                    }
                    if (count == 0)
                        break;
                    size_t written = 0;
                    while (written < static_cast<size_t>(count))
                    {
                        const ssize_t result =
                            ::write(output, buffer.data() + written, static_cast<size_t>(count) - written);
                        if (result < 0 && errno == EINTR)
                            continue;
                        if (result <= 0)
                        {
                            copied = false;
                            break;
                        }
                        written += static_cast<size_t>(result);
                    }
                    if (!copied)
                        break;
                }
                if (copied && ::fsync(output) != 0)
                    copied = false;
                ::close(output);
                ::close(input);
                if (!copied)
                {
                    error = "failed to copy the complete plugin image into private staging";
                    Cleanup();
                    return false;
                }
                return true;
            }

            [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }
            std::filesystem::path ReleaseDirectory()
            {
                m_path.clear();
                return std::exchange(m_directory, {});
            }

          private:
            void Cleanup()
            {
                if (m_directory.empty())
                    return;
                std::error_code ignored;
                std::filesystem::remove_all(m_directory, ignored);
                m_directory.clear();
                m_path.clear();
            }

            std::filesystem::path m_directory;
            std::filesystem::path m_path;
        };

        bool CapturePluginFileIdentity(const std::filesystem::path& path, PluginFileIdentity& identity,
                                       std::string& error)
        {
            struct stat information = {};
            if (::stat(path.c_str(), &information) != 0 || !S_ISREG(information.st_mode))
            {
                error = "failed to query plugin file identity";
                return false;
            }
            identity.device = information.st_dev;
            identity.inode = information.st_ino;
            identity.size = static_cast<uint64_t>(information.st_size);
            return true;
        }
#endif

        bool SamePluginFile(const PluginFileIdentity& left, const PluginFileIdentity& right)
        {
#if defined(_WIN32)
            return left.volumeSerial == right.volumeSerial && left.fileIndexHigh == right.fileIndexHigh &&
                   left.fileIndexLow == right.fileIndexLow && left.size == right.size;
#else
            return left.device == right.device && left.inode == right.inode && left.size == right.size;
#endif
        }

        std::string PathToUtf8(const std::filesystem::path& path)
        {
            const std::u8string utf8 = path.generic_u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        std::filesystem::path MetadataPath(const std::filesystem::path& pluginPath)
        {
            std::filesystem::path metadata = pluginPath;
            metadata += ".sparkplugin.json";
            return metadata;
        }

        bool ReadRequiredString(const Json::Value& root, std::string_view name, std::string& value, std::string& error)
        {
            const Json::Value& field = root[std::string(name)];
            if (!field.IsString() || field.AsString().empty())
            {
                error = "missing or invalid string field '" + std::string(name) + "'";
                return false;
            }
            value = field.AsString();
            return true;
        }

        bool ReadRequiredUInt(const Json::Value& root, std::string_view name, uint32_t& value, std::string& error)
        {
            const Json::Value& field = root[std::string(name)];
            if (!field.IsNumber())
            {
                error = "missing or invalid integer field '" + std::string(name) + "'";
                return false;
            }
            const double number = field.AsNumber();
            if (!std::isfinite(number) || number < 0.0 || number > static_cast<double>(UINT32_MAX) ||
                std::floor(number) != number)
            {
                error = "missing or invalid integer field '" + std::string(name) + "'";
                return false;
            }
            value = static_cast<uint32_t>(number);
            return true;
        }

        bool ValidatePluginMetadata(const std::filesystem::path& pluginPath, PluginMetadata& metadata,
                                    std::string& error)
        {
            constexpr uintmax_t kMaximumMetadataBytes = 64 * 1024;
            const std::filesystem::path metadataPath = MetadataPath(pluginPath);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(metadataPath, ec) || ec)
            {
                error = "missing mandatory plugin metadata '" + PathToUtf8(metadataPath) + "'";
                return false;
            }
            const uintmax_t metadataSize = std::filesystem::file_size(metadataPath, ec);
            if (ec || metadataSize == 0 || metadataSize > kMaximumMetadataBytes)
            {
                error = "plugin metadata has an invalid size";
                return false;
            }

            std::ifstream stream(metadataPath, std::ios::binary);
            if (!stream)
            {
                error = "failed to open plugin metadata";
                return false;
            }
            std::ostringstream contents;
            contents << stream.rdbuf();
            if (!stream.eof() && stream.fail())
            {
                error = "failed to read plugin metadata";
                return false;
            }

            Json::Value root;
            std::string parseError;
            const std::string json = contents.str();
            if (!Json::ParseStrict(json, &root, &parseError) || !root.IsObject())
            {
                error = "malformed plugin metadata: " + parseError;
                return false;
            }
            if (root.Size() != 9)
            {
                error = "plugin metadata must contain exactly the schema 1 fields";
                return false;
            }

            uint32_t schema = 0;
            uint32_t abiMajor = 0;
            uint32_t abiMinor = 0;
            std::string type;
            std::string entryPoint;
            std::string binary;
            if (!ReadRequiredUInt(root, "schema", schema, error) ||
                !ReadRequiredString(root, "id", metadata.id, error) ||
                !ReadRequiredString(root, "version", metadata.version, error) ||
                !ReadRequiredString(root, "type", type, error) ||
                !ReadRequiredUInt(root, "abi_major", abiMajor, error) ||
                !ReadRequiredUInt(root, "abi_minor", abiMinor, error) ||
                !ReadRequiredString(root, "entry_point", entryPoint, error) ||
                !ReadRequiredString(root, "binary", binary, error) ||
                !ReadRequiredString(root, "sha256", metadata.expectedHash, error))
            {
                return false;
            }

            if (schema != 1)
            {
                error = "unsupported plugin metadata schema";
                return false;
            }
            if (abiMajor != SPARK_PLUGIN_ABI_MAJOR || abiMinor > SPARK_PLUGIN_ABI_MINOR)
            {
                error = "plugin metadata ABI is incompatible with this host";
                return false;
            }
            if (entryPoint != SPARK_PLUGIN_ENTRY_POINT)
            {
                error = "plugin metadata entry point mismatch";
                return false;
            }
            if (binary != PathToUtf8(pluginPath.filename()))
            {
                error = "plugin metadata binary name mismatch";
                return false;
            }
            if (metadata.expectedHash.size() != 64 ||
                !std::all_of(metadata.expectedHash.begin(), metadata.expectedHash.end(),
                             [](unsigned char c) { return std::isxdigit(c); }))
            {
                error = "plugin metadata has an invalid SHA-256 digest";
                return false;
            }
            std::transform(metadata.expectedHash.begin(), metadata.expectedHash.end(), metadata.expectedHash.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            std::string actualHash;
            std::string hashError;
            if (!FileIntegrity::ComputeSha256(pluginPath, actualHash, hashError))
            {
                error = "failed to hash plugin binary: " + hashError;
                return false;
            }
            if (actualHash != metadata.expectedHash)
            {
                error = "plugin metadata binary SHA-256 mismatch";
                return false;
            }
            return true;
        }
    } // namespace

    struct DynamicPluginHost::Impl
    {
        struct TaskState
        {
            mutable std::mutex mutex;
            std::condition_variable complete;
            std::thread worker;
            bool done = false;
            bool failed = false;
        };

        SparkPluginHostAPI host{};
        const SparkPluginDescriptor* descriptor = nullptr;
        SparkPluginInstance instance = 0;
        State state = State::Empty;
        std::filesystem::path path;
        std::filesystem::path stagedDirectory;
        void* library = nullptr;
        bool ownsLibrary = false;
        LogSink logSink;
        ResourceResolver resolver;

        mutable std::mutex tasksMutex;
        std::unordered_map<SparkPluginTask, std::shared_ptr<TaskState>> tasks;
        std::atomic<uint64_t> nextTask{1};
        bool unloading = false;
        inline static thread_local SparkPluginTask executingTask = 0;

        Impl()
        {
            host.struct_size = sizeof(host);
            host.abi_major = SPARK_PLUGIN_ABI_MAJOR;
            host.abi_minor = SPARK_PLUGIN_ABI_MINOR;
            host.host_context = this;
            host.allocate = &AllocateThunk;
            host.reallocate = &ReallocateThunk;
            host.deallocate = &DeallocateThunk;
            host.log = &LogThunk;
            host.schedule_task = &ScheduleTaskThunk;
            host.wait_task = &WaitTaskThunk;
            host.cancel_task = &CancelTaskThunk;
            host.resolve_resource = &ResolveResourceThunk;
        }

        static void* AllocateThunk(void*, uint64_t size, uint64_t alignment, uint32_t)
        {
            return HostAllocate(size, alignment);
        }

        static void* ReallocateThunk(void*, void* memory, uint64_t size, uint64_t alignment, uint32_t)
        {
            if (!memory)
                return HostAllocate(size, alignment);
            if (size == 0)
            {
                HostDeallocate(memory);
                return nullptr;
            }
            const auto* oldHeader = reinterpret_cast<const AllocationHeader*>(memory) - 1;
            const uint64_t oldSize = oldHeader->size;
            void* replacement = HostAllocate(size, alignment);
            if (!replacement)
                return nullptr;
            std::memcpy(replacement, memory, static_cast<size_t>(std::min(oldSize, size)));
            HostDeallocate(memory);
            return replacement;
        }

        static void DeallocateThunk(void*, void* memory, uint64_t, uint32_t) { HostDeallocate(memory); }

        static void LogThunk(void* context, SparkPluginLogLevel level, const char* category, const char* message)
        {
            if (!context)
                return;
            try
            {
                auto& self = *static_cast<Impl*>(context);
                if (self.logSink)
                    self.logSink(level, category ? category : "Plugin", message ? message : "");
            }
            catch (...)
            {
                // Logging is advisory. A throwing host sink must never unwind
                // through the plugin's C ABI frame.
            }
        }

        static SparkPluginResult ScheduleTaskThunk(void* context, SparkPluginTaskFn callback, void* taskContext,
                                                   SparkPluginTask* outTask)
        {
            if (!context || !callback || !outTask)
                return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
            *outTask = 0;
            auto& self = *static_cast<Impl*>(context);
            SparkPluginTask id = 0;
            bool inserted = false;
            try
            {
                auto task = std::make_shared<TaskState>();
                id = self.nextTask.fetch_add(1, std::memory_order_relaxed);
                if (id == 0)
                    return SPARK_PLUGIN_ERROR_INTERNAL;
                {
                    std::lock_guard lock(self.tasksMutex);
                    if (self.unloading || self.tasks.size() >= 64)
                        return SPARK_PLUGIN_ERROR_BUSY;
                    self.tasks.emplace(id, task);
                    inserted = true;
                }
                task->worker = std::thread(
                    [task, callback, taskContext, id]
                    {
                        bool failed = false;
                        executingTask = id;
                        try
                        {
                            callback(taskContext);
                        }
                        catch (...)
                        {
                            // Exceptions are forbidden across the ABI. Contain a violating
                            // C++ plugin so it cannot tear down the host process.
                            failed = true;
                        }
                        executingTask = 0;
                        {
                            std::lock_guard lock(task->mutex);
                            task->failed = failed;
                            task->done = true;
                        }
                        task->complete.notify_all();
                    });
                *outTask = id;
                return SPARK_PLUGIN_OK;
            }
            catch (const std::bad_alloc&)
            {
                if (inserted)
                {
                    std::lock_guard lock(self.tasksMutex);
                    self.tasks.erase(id);
                }
                return SPARK_PLUGIN_ERROR_OUT_OF_MEMORY;
            }
            catch (...)
            {
                if (inserted)
                {
                    std::lock_guard lock(self.tasksMutex);
                    self.tasks.erase(id);
                }
                return SPARK_PLUGIN_ERROR_INTERNAL;
            }
        }

        static SparkPluginResult WaitTaskThunk(void* context, SparkPluginTask id, uint32_t timeoutMs)
        {
            if (!context || id == 0)
                return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
            if (executingTask == id)
                return SPARK_PLUGIN_ERROR_BUSY;
            auto& self = *static_cast<Impl*>(context);
            std::shared_ptr<TaskState> task;
            {
                std::lock_guard lock(self.tasksMutex);
                const auto it = self.tasks.find(id);
                if (it == self.tasks.end())
                    return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
                task = it->second;
            }
            {
                std::unique_lock lock(task->mutex);
                const bool finished = timeoutMs == UINT32_MAX
                                          ? (task->complete.wait(lock, [&] { return task->done; }), true)
                                          : task->complete.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                                                    [&] { return task->done; });
                if (!finished)
                    return SPARK_PLUGIN_ERROR_TIMEOUT;
                if (task->worker.joinable())
                    task->worker.join();
                const bool failed = task->failed;
                lock.unlock();
                {
                    std::lock_guard tasksLock(self.tasksMutex);
                    self.tasks.erase(id);
                }
                return failed ? SPARK_PLUGIN_ERROR_INTERNAL : SPARK_PLUGIN_OK;
            }
        }

        static SparkPluginResult CancelTaskThunk(void* context, SparkPluginTask id)
        {
            if (!context || id == 0)
                return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
            auto& self = *static_cast<Impl*>(context);
            std::lock_guard lock(self.tasksMutex);
            return self.tasks.contains(id) ? SPARK_PLUGIN_ERROR_BUSY : SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        }

        static SparkPluginResult ResolveResourceThunk(void* context, const char* stableId,
                                                      SparkPluginResource* outResource)
        {
            if (!context || !stableId || !outResource)
                return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
            *outResource = 0;
            try
            {
                auto& self = *static_cast<Impl*>(context);
                return self.resolver ? self.resolver(stableId, outResource) : SPARK_PLUGIN_ERROR_UNSUPPORTED;
            }
            catch (const std::bad_alloc&)
            {
                return SPARK_PLUGIN_ERROR_OUT_OF_MEMORY;
            }
            catch (...)
            {
                return SPARK_PLUGIN_ERROR_INTERNAL;
            }
        }

        size_t ActiveTaskCount() const
        {
            std::lock_guard lock(tasksMutex);
            return tasks.size();
        }

        void BeginUnload()
        {
            std::lock_guard lock(tasksMutex);
            unloading = true;
        }

        void CancelUnload()
        {
            std::lock_guard lock(tasksMutex);
            unloading = false;
        }

        bool WaitForTasks(std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            for (;;)
            {
                std::shared_ptr<TaskState> task;
                {
                    std::lock_guard lock(tasksMutex);
                    if (tasks.empty())
                        return true;
                    task = tasks.begin()->second;
                }
                std::unique_lock lock(task->mutex);
                if (!task->complete.wait_until(lock, deadline, [&] { return task->done; }))
                    return false;
                if (task->worker.joinable())
                    task->worker.join();
                lock.unlock();
                std::lock_guard tasksLock(tasksMutex);
                for (auto it = tasks.begin(); it != tasks.end(); ++it)
                {
                    if (it->second == task)
                    {
                        tasks.erase(it);
                        break;
                    }
                }
            }
        }

        bool Attach(const SparkPluginDescriptor* candidate, std::string* error,
                    const PluginMetadata* metadata = nullptr)
        {
            const SparkPluginResult validation = SparkValidatePluginDescriptor(candidate);
            if (validation != SPARK_PLUGIN_OK)
            {
                if (error)
                    *error = ResultMessage("descriptor validation", validation);
                return false;
            }
            if (metadata &&
                (metadata->id != candidate->id || metadata->version != (candidate->version ? candidate->version : "")))
            {
                if (error)
                    *error = "plugin metadata identity does not match the in-image descriptor";
                return false;
            }
            // A successfully unloaded host keeps scheduling closed until a new,
            // fully validated descriptor is about to create its instance.
            CancelUnload();
            SparkPluginInstance created = 0;
            SparkPluginResult result = SPARK_PLUGIN_ERROR_INTERNAL;
            try
            {
                result = candidate->api->create(&host, &created);
            }
            catch (...)
            {
                BeginUnload();
                if (error)
                    *error = "plugin create threw across the C ABI";
                return false;
            }
            if (result != SPARK_PLUGIN_OK || created == 0)
            {
                BeginUnload();
                if (error)
                    *error = result == SPARK_PLUGIN_OK ? "plugin create returned a null instance"
                                                       : ResultMessage("plugin create", result);
                return false;
            }
            descriptor = candidate;
            instance = created;
            state = State::Loaded;
            return true;
        }

        void CloseLibrary()
        {
            if (ownsLibrary && library)
            {
#if defined(_WIN32)
                ::FreeLibrary(static_cast<HMODULE>(library));
#else
                ::dlclose(library);
#endif
                library = nullptr;
                ownsLibrary = false;
            }
            if (!stagedDirectory.empty())
            {
                std::error_code ignored;
                std::filesystem::remove_all(stagedDirectory, ignored);
                stagedDirectory.clear();
            }
        }
    };

    DynamicPluginHost::DynamicPluginHost() : m_impl(std::make_unique<Impl>()) {}

    DynamicPluginHost::~DynamicPluginHost()
    {
        if (!m_impl)
            return;
        if (m_impl->descriptor)
        {
            std::string ignored;
            if (!Unload(std::chrono::seconds(5), &ignored))
            {
                // Fail closed without turning object destruction into an unbounded
                // wait: a plugin that violates its unload fence must remain mapped,
                // together with the host state referenced by any outstanding task.
                // The operating system reclaims this deliberately leaked quarantine
                // at process exit.
                (void)m_impl.release();
            }
        }
        else if (m_impl->ActiveTaskCount() != 0 && !m_impl->WaitForTasks(std::chrono::seconds(5)))
        {
            // A failed create is permitted to have scheduled work. Quarantine
            // the host and mapped image if that work ignores the same bound.
            (void)m_impl.release();
        }
        else
        {
            m_impl->CloseLibrary();
        }
    }

    void DynamicPluginHost::SetLogSink(LogSink sink)
    {
        m_impl->logSink = std::move(sink);
    }

    void DynamicPluginHost::SetResourceResolver(ResourceResolver resolver)
    {
        m_impl->resolver = std::move(resolver);
    }

    bool DynamicPluginHost::Load(const std::filesystem::path& path, std::string* error)
    {
        if (IsLoaded() || m_impl->library != nullptr || m_impl->ActiveTaskCount() != 0 || m_impl->state != State::Empty)
        {
            if (error)
                *error = "the plugin host is already loaded or quarantined";
            return false;
        }
        std::error_code ec;
        const auto absolute = std::filesystem::weakly_canonical(path, ec);
        if (ec || !absolute.is_absolute() || !std::filesystem::is_regular_file(absolute, ec))
        {
            if (error)
                *error = "plugin path is not an existing absolute regular file";
            return false;
        }

        PluginFileIdentity initialIdentity;
        std::string identityError;
        std::filesystem::path loadPath = absolute;
#if defined(_WIN32)
        StablePluginFile stableFile;
        if (!stableFile.Open(absolute, identityError) ||
            !CapturePluginFileIdentity(stableFile.Get(), initialIdentity, identityError))
#else
        if (!CapturePluginFileIdentity(absolute, initialIdentity, identityError))
#endif
        {
            if (error)
                *error = std::move(identityError);
            return false;
        }

        PluginMetadata metadata;
        std::string metadataError;
        if (!ValidatePluginMetadata(absolute, metadata, metadataError))
        {
            if (error)
                *error = std::move(metadataError);
            return false;
        }

#if !defined(_WIN32)
        PrivatePluginCopy privateCopy;
        if (!privateCopy.Create(absolute, identityError))
        {
            if (error)
                *error = std::move(identityError);
            return false;
        }
        loadPath = privateCopy.Path();
        std::string stagedHash;
        if (!CapturePluginFileIdentity(loadPath, initialIdentity, identityError) ||
            !FileIntegrity::ComputeSha256(loadPath, stagedHash, identityError) || stagedHash != metadata.expectedHash)
        {
            if (error)
                *error = identityError.empty() ? "private staged plugin image failed SHA-256 validation"
                                               : std::move(identityError);
            return false;
        }
#endif

        PluginFileIdentity hashedIdentity;
        if (!CapturePluginFileIdentity(loadPath, hashedIdentity, identityError) ||
            !SamePluginFile(initialIdentity, hashedIdentity))
        {
            if (error)
                *error = identityError.empty() ? "plugin binary identity changed while it was hashed"
                                               : std::move(identityError);
            return false;
        }

#if defined(_WIN32)
        HMODULE module = ::LoadLibraryExW(loadPath.c_str(), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module)
        {
            if (error)
                *error = "LoadLibraryExW failed with error " + std::to_string(::GetLastError());
            return false;
        }
        auto entry = reinterpret_cast<SparkGetPluginDescriptorFn>(::GetProcAddress(module, SPARK_PLUGIN_ENTRY_POINT));
#else
        void* module = ::dlopen(loadPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module)
        {
            if (error)
            {
                const char* detail = ::dlerror();
                *error = detail ? detail : "dlopen failed";
            }
            return false;
        }
        auto entry = reinterpret_cast<SparkGetPluginDescriptorFn>(::dlsym(module, SPARK_PLUGIN_ENTRY_POINT));
#endif
        if (!entry)
        {
            if (error)
                *error = "missing " SPARK_PLUGIN_ENTRY_POINT " export";
#if defined(_WIN32)
            ::FreeLibrary(module);
#else
            ::dlclose(module);
#endif
            return false;
        }


        std::filesystem::path mappedPath;
#if defined(_WIN32)
        std::wstring mappedPathBuffer(32768, L'\0');
        const DWORD mappedPathLength =
            ::GetModuleFileNameW(module, mappedPathBuffer.data(), static_cast<DWORD>(mappedPathBuffer.size()));
        if (mappedPathLength == 0 || mappedPathLength >= mappedPathBuffer.size())
        {
            if (error)
                *error = "failed to identify the mapped plugin image";
            ::FreeLibrary(module);
            return false;
        }
        mappedPathBuffer.resize(mappedPathLength);
        mappedPath = std::filesystem::path(mappedPathBuffer);
#else
        Dl_info mappedInformation{};
        if (::dladdr(reinterpret_cast<void*>(entry), &mappedInformation) == 0 || !mappedInformation.dli_fname)
        {
            if (error)
                *error = "failed to identify the mapped plugin image";
            ::dlclose(module);
            return false;
        }
        mappedPath = mappedInformation.dli_fname;
#endif

        PluginFileIdentity mappedIdentity;
        std::string postMapError;
        std::string mappedHash;
        if (!CapturePluginFileIdentity(mappedPath, mappedIdentity, postMapError) ||
            !SamePluginFile(initialIdentity, mappedIdentity) ||
            !FileIntegrity::ComputeSha256(mappedPath, mappedHash, postMapError) || mappedHash != metadata.expectedHash)
        {
            if (error)
                *error = postMapError.empty() ? "mapped plugin image does not match the validated binary"
                                              : "failed to validate mapped plugin image: " + postMapError;
#if defined(_WIN32)
            ::FreeLibrary(module);
#else
            ::dlclose(module);
#endif
            return false;
        }

        const SparkPluginDescriptor* candidate = nullptr;
        try
        {
            candidate = entry(SPARK_PLUGIN_ABI_MAJOR, SPARK_PLUGIN_ABI_MINOR);
        }
        catch (...)
        {
            if (error)
                *error = "plugin descriptor entry point threw across the C ABI";
#if defined(_WIN32)
            ::FreeLibrary(module);
#else
            ::dlclose(module);
#endif
            return false;
        }

        m_impl->library = module;
        m_impl->ownsLibrary = true;
        m_impl->path = absolute;
#if !defined(_WIN32)
        m_impl->stagedDirectory = privateCopy.ReleaseDirectory();
#endif
        if (!m_impl->Attach(candidate, error, &metadata))
        {
            if (m_impl->WaitForTasks(std::chrono::seconds(5)))
            {
                m_impl->CloseLibrary();
                m_impl->path.clear();
                m_impl->CancelUnload();
            }
            else if (error)
            {
                *error += "; failed-create tasks did not reach the unload fence, image quarantined";
            }
            return false;
        }
        return true;
    }

    bool DynamicPluginHost::AttachDescriptorForTesting(const SparkPluginDescriptor* descriptor, std::string* error)
    {
        if (IsLoaded() || m_impl->library != nullptr || m_impl->ActiveTaskCount() != 0 || m_impl->state != State::Empty)
        {
            if (error)
                *error = "the plugin host is already loaded or quarantined";
            return false;
        }
        if (m_impl->Attach(descriptor, error))
            return true;
        if (m_impl->WaitForTasks(std::chrono::seconds(5)))
            m_impl->CancelUnload();
        return false;
    }

    bool DynamicPluginHost::Start(std::string* error)
    {
        if (m_impl->state != State::Loaded && m_impl->state != State::Stopped)
        {
            if (error)
                *error = "plugin is not in a startable state";
            return false;
        }
        const auto start = m_impl->descriptor->api->start;
        if (start)
        {
            SparkPluginResult result = SPARK_PLUGIN_ERROR_INTERNAL;
            try
            {
                result = start(m_impl->instance);
            }
            catch (...)
            {
                if (error)
                    *error = "plugin start threw across the C ABI";
                return false;
            }
            if (result != SPARK_PLUGIN_OK)
            {
                if (error)
                    *error = ResultMessage("plugin start", result);
                return false;
            }
        }
        m_impl->state = State::Started;
        return true;
    }

    SparkPluginResult DynamicPluginHost::Tick(double deltaSeconds)
    {
        if (m_impl->state != State::Started || !std::isfinite(deltaSeconds) || deltaSeconds < 0.0)
            return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
        const auto* api = m_impl->descriptor->api;
        if ((api->capabilities & SPARK_PLUGIN_CAP_TICK) == 0 || !api->tick)
            return SPARK_PLUGIN_ERROR_UNSUPPORTED;
        try
        {
            return api->tick(m_impl->instance, deltaSeconds);
        }
        catch (...)
        {
            return SPARK_PLUGIN_ERROR_INTERNAL;
        }
    }

    bool DynamicPluginHost::Reload(const std::filesystem::path& replacementPath, std::chrono::milliseconds timeout,
                                   std::string* error)
    {
        if (!IsLoaded())
        {
            if (error)
                *error = "no plugin is loaded to reload";
            return false;
        }

        DynamicPluginHost replacement;
        replacement.SetLogSink(m_impl->logSink);
        replacement.SetResourceResolver(m_impl->resolver);
        std::string replacementError;
        if (!replacement.Load(replacementPath, &replacementError))
        {
            if (error)
                *error = "replacement load failed: " + replacementError;
            return false;
        }
        return CommitReload(replacement, timeout, error);
    }

    bool DynamicPluginHost::ReloadDescriptorForTesting(const SparkPluginDescriptor* descriptor,
                                                       std::chrono::milliseconds timeout, std::string* error)
    {
        if (!IsLoaded())
        {
            if (error)
                *error = "no plugin is loaded to reload";
            return false;
        }

        DynamicPluginHost replacement;
        replacement.SetLogSink(m_impl->logSink);
        replacement.SetResourceResolver(m_impl->resolver);
        std::string replacementError;
        if (!replacement.AttachDescriptorForTesting(descriptor, &replacementError))
        {
            if (error)
                *error = "replacement descriptor failed: " + replacementError;
            return false;
        }
        return CommitReload(replacement, timeout, error);
    }

    bool DynamicPluginHost::CommitReload(DynamicPluginHost& replacement, std::chrono::milliseconds timeout,
                                         std::string* error)
    {
        const auto* oldDescriptor = m_impl->descriptor;
        const auto* replacementDescriptor = replacement.m_impl->descriptor;
        if (!HasHotReloadContract(oldDescriptor) || !HasHotReloadContract(replacementDescriptor))
        {
            if (error)
                *error = "both plugins must advertise transactional hot reload and provide "
                         "prepare_unload/save_state/restore_state/cancel_unload";
            return false;
        }
        if (std::strcmp(oldDescriptor->id, replacementDescriptor->id) != 0)
        {
            if (error)
                *error = "replacement plugin id does not match the loaded plugin";
            return false;
        }

        std::vector<uint8_t> state;
        try
        {
            state.resize(static_cast<size_t>(kMaximumReloadStateBytes));
        }
        catch (...)
        {
            if (error)
                *error = "failed to allocate the bounded hot-reload state buffer";
            return false;
        }

        const State oldState = m_impl->state;
        if (!PrepareUnload(timeout, error))
            return false;

        const auto rollbackPrepared = [&](std::string reason)
        {
            SparkPluginResult cancelResult = SPARK_PLUGIN_ERROR_INTERNAL;
            try
            {
                cancelResult = oldDescriptor->api->cancel_unload(m_impl->instance);
            }
            catch (...)
            {
                if (error)
                    *error = std::move(reason) + "; plugin cancel_unload threw, so the old plugin remains quarantined";
                return false;
            }
            if (cancelResult != SPARK_PLUGIN_OK)
            {
                if (error)
                    *error = std::move(reason) + "; " + ResultMessage("plugin cancel_unload", cancelResult) +
                             ", so the old plugin remains quarantined";
                return false;
            }
            m_impl->CancelUnload();
            if (error)
                *error = std::move(reason);
            return false;
        };

        SparkPluginMutableBytes output{state.data(), 0, kMaximumReloadStateBytes};
        SparkPluginResult saveResult = SPARK_PLUGIN_ERROR_INTERNAL;
        try
        {
            saveResult = oldDescriptor->api->save_state(m_impl->instance, &output);
        }
        catch (...)
        {
            return rollbackPrepared("plugin save_state threw across the C ABI");
        }
        if (saveResult != SPARK_PLUGIN_OK)
            return rollbackPrepared(ResultMessage("plugin save_state", saveResult));
        if (output.data != state.data() || output.capacity != kMaximumReloadStateBytes || output.size > output.capacity)
            return rollbackPrepared("plugin save_state violated the host-owned bounded buffer contract");
        state.resize(static_cast<size_t>(output.size));

        const SparkPluginBytes input{state.data(), static_cast<uint64_t>(state.size())};
        SparkPluginResult restoreResult = SPARK_PLUGIN_ERROR_INTERNAL;
        try
        {
            restoreResult = replacementDescriptor->api->restore_state(replacement.m_impl->instance, input);
        }
        catch (...)
        {
            return rollbackPrepared("replacement restore_state threw across the C ABI");
        }
        if (restoreResult != SPARK_PLUGIN_OK)
            return rollbackPrepared(ResultMessage("replacement restore_state", restoreResult));

        if (oldState == State::Started)
        {
            try
            {
                if (oldDescriptor->api->stop)
                    oldDescriptor->api->stop(m_impl->instance);
                m_impl->state = State::Stopped;
            }
            catch (...)
            {
                if (error)
                    *error = "old plugin stop threw during reload commit; the old plugin remains quarantined";
                return false;
            }

            std::string startError;
            if (!replacement.Start(&startError))
            {
                SparkPluginResult restartResult = SPARK_PLUGIN_ERROR_INTERNAL;
                try
                {
                    restartResult =
                        oldDescriptor->api->start ? oldDescriptor->api->start(m_impl->instance) : SPARK_PLUGIN_OK;
                }
                catch (...)
                {
                    if (error)
                        *error = "replacement start failed: " + startError +
                                 "; restarting the old plugin threw, so it remains quarantined";
                    return false;
                }
                if (restartResult != SPARK_PLUGIN_OK)
                {
                    if (error)
                        *error = "replacement start failed: " + startError + "; " +
                                 ResultMessage("old plugin restart", restartResult) +
                                 ", so the old plugin remains quarantined";
                    return false;
                }
                m_impl->state = State::Started;
                return rollbackPrepared("replacement start failed: " + startError);
            }
        }
        if (oldState == State::Stopped)
            replacement.m_impl->state = State::Stopped;

        // Commit only after the replacement has restored and reached the old
        // lifecycle state. If old teardown fails, replacement remains owned by
        // its temporary host and the old host stays fail-closed/quarantined.
        if (!FinishUnload(timeout, error))
            return false;

        m_impl.swap(replacement.m_impl);
        return true;
    }

    bool DynamicPluginHost::PrepareUnload(std::chrono::milliseconds timeout, std::string* error)
    {
        if (!IsLoaded())
            return true;
        m_impl->BeginUnload();
        const auto* api = m_impl->descriptor->api;
        if (api->prepare_unload)
        {
            const auto bounded = std::clamp<int64_t>(timeout.count(), 0, UINT32_MAX);
            SparkPluginResult result = SPARK_PLUGIN_ERROR_INTERNAL;
            try
            {
                result = api->prepare_unload(m_impl->instance, static_cast<uint32_t>(bounded));
            }
            catch (...)
            {
                m_impl->CancelUnload();
                if (error)
                    *error = "plugin prepare_unload threw across the C ABI";
                return false;
            }
            if (result != SPARK_PLUGIN_OK)
            {
                m_impl->CancelUnload();
                if (error)
                    *error = ResultMessage("plugin prepare_unload", result);
                return false;
            }
        }
        if (!m_impl->WaitForTasks(timeout))
        {
            if (api->cancel_unload)
            {
                SparkPluginResult cancelResult = SPARK_PLUGIN_ERROR_INTERNAL;
                try
                {
                    cancelResult = api->cancel_unload(m_impl->instance);
                }
                catch (...)
                {
                    if (error)
                        *error = "plugin host task fence timed out; plugin cancel_unload threw, so the plugin remains "
                                 "quarantined";
                    return false;
                }
                if (cancelResult != SPARK_PLUGIN_OK)
                {
                    if (error)
                        *error = "plugin host task fence timed out; " +
                                 ResultMessage("plugin cancel_unload", cancelResult) +
                                 ", so the plugin remains quarantined";
                    return false;
                }
            }
            m_impl->CancelUnload();
            if (error)
                *error = "plugin host task fence timed out";
            return false;
        }
        return true;
    }

    bool DynamicPluginHost::FinishUnload(std::chrono::milliseconds timeout, std::string* error)
    {
        if (!IsLoaded())
            return true;
        const auto* api = m_impl->descriptor->api;
        if (m_impl->state == State::Started && api->stop)
        {
            try
            {
                api->stop(m_impl->instance);
            }
            catch (...)
            {
                if (error)
                    *error = "plugin stop threw across the C ABI";
                return false;
            }
        }
        m_impl->state = State::Stopped;
        // stop is plugin code too. Keep the gate closed and fence again before
        // destroy/unmap in case a callback was already entering the scheduler.
        if (!m_impl->WaitForTasks(timeout))
        {
            if (error)
                *error = "plugin host post-stop task fence timed out";
            return false;
        }
        try
        {
            api->destroy(m_impl->instance);
        }
        catch (...)
        {
            if (error)
                *error = "plugin destroy threw across the C ABI";
            return false;
        }
        m_impl->instance = 0;
        m_impl->descriptor = nullptr;
        m_impl->state = State::Empty;
        m_impl->CloseLibrary();
        m_impl->path.clear();
        m_impl->CancelUnload();
        return true;
    }

    bool DynamicPluginHost::Unload(std::chrono::milliseconds timeout, std::string* error)
    {
        return PrepareUnload(timeout, error) && FinishUnload(timeout, error);
    }

    DynamicPluginHost::State DynamicPluginHost::GetState() const noexcept
    {
        return m_impl->state;
    }
    bool DynamicPluginHost::IsLoaded() const noexcept
    {
        return m_impl->descriptor != nullptr;
    }
    const SparkPluginDescriptor* DynamicPluginHost::Descriptor() const noexcept
    {
        return m_impl->descriptor;
    }
    const std::filesystem::path& DynamicPluginHost::Path() const noexcept
    {
        return m_impl->path;
    }
    size_t DynamicPluginHost::ActiveTaskCount() const
    {
        return m_impl->ActiveTaskCount();
    }
} // namespace Spark
