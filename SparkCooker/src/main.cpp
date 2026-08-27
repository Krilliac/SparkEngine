#include "SparkAssetPipelineCore/AssetCooker.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    struct WorkerJob
    {
        std::filesystem::path source;
        std::filesystem::path output;
        std::string portablePath;
        std::string sha256;
    };

    class ScratchDirectory final
    {
      public:
        explicit ScratchDirectory(std::filesystem::path path) : m_path(std::move(path)) {}
        ~ScratchDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(m_path, ignored);
        }
        ScratchDirectory(const ScratchDirectory&) = delete;
        ScratchDirectory& operator=(const ScratchDirectory&) = delete;
        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

      private:
        std::filesystem::path m_path;
    };

    void PrintUsage()
    {
        std::cout << "Usage: SparkCooker --source <dir> --output <dir> [--manifest <file>] [--dry-run] "
                     "[--worker <SparkWorker>] [--jobs <1-64>]\n";
    }

    bool ParseJobCount(std::string_view value, std::size_t& jobs)
    {
        unsigned parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
        if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 || parsed > 64)
            return false;
        jobs = parsed;
        return true;
    }

    bool IsContained(const std::filesystem::path& child, const std::filesystem::path& parent)
    {
        const auto relative = child.lexically_relative(parent);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
    }

    bool IsLinkLike(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)))
            return true;
#if defined(_WIN32)
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
        return false;
#endif
    }

    bool PortableRelative(const std::filesystem::path& path, const std::filesystem::path& root, std::string& output)
    {
        try
        {
            const std::u8string utf8 = path.lexically_relative(root).generic_u8string();
            output.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            return !output.empty();
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return false;
        }
    }

    bool CreateScratchDirectory(std::filesystem::path& path, std::string& error)
    {
        std::error_code ec;
        const auto root = std::filesystem::temp_directory_path(ec);
        if (ec)
        {
            error = "failed to resolve worker scratch root: " + ec.message();
            return false;
        }
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::uint32_t attempt = 0; attempt < 32; ++attempt)
        {
            path = root / ("spark-cooker-workers-" + std::to_string(nonce) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path, ec))
                return true;
            if (ec && ec != std::errc::file_exists)
            {
                error = "failed to create worker scratch directory: " + ec.message();
                return false;
            }
            ec.clear();
        }
        error = "failed to allocate a unique worker scratch directory";
        return false;
    }

    bool BuildWorkerJobs(const std::filesystem::path& requestedSource, const std::filesystem::path& scratch,
                         std::vector<WorkerJob>& jobs, std::string& error)
    {
        std::error_code ec;
        const auto requestedStatus = std::filesystem::symlink_status(requestedSource, ec);
        if (ec || std::filesystem::is_symlink(requestedStatus) || IsLinkLike(requestedSource))
        {
            error = "worker source root must not be a symbolic link";
            return false;
        }
        const auto source = std::filesystem::weakly_canonical(requestedSource, ec);
        if (ec || !std::filesystem::is_directory(source))
        {
            error = "worker source root is not a readable directory";
            return false;
        }

        std::filesystem::recursive_directory_iterator iterator(source, ec), end;
        while (!ec && iterator != end)
        {
            const auto status = iterator->symlink_status(ec);
            if (ec)
                break;
            if (std::filesystem::is_symlink(status) || IsLinkLike(iterator->path()))
            {
                iterator.disable_recursion_pending();
            }
            else if (std::filesystem::is_regular_file(status))
            {
                const auto canonical = std::filesystem::weakly_canonical(iterator->path(), ec);
                if (ec || !IsContained(canonical, source))
                {
                    error = "worker source entry escapes the source root";
                    return false;
                }
                WorkerJob job;
                job.source = canonical;
                if (!PortableRelative(canonical, source, job.portablePath))
                {
                    error = "worker source path is not representable as portable UTF-8";
                    return false;
                }
                job.output = scratch / std::filesystem::u8path(job.portablePath);
                jobs.push_back(std::move(job));
            }
            iterator.increment(ec);
        }
        if (ec)
        {
            error = "failed to enumerate worker source assets: " + ec.message();
            return false;
        }
        std::sort(jobs.begin(), jobs.end(),
                  [](const WorkerJob& left, const WorkerJob& right) { return left.portablePath < right.portablePath; });
        for (auto& job : jobs)
        {
            if (!Spark::AssetPipeline::ComputeFileSha256(job.source, job.sha256, error))
                return false;
        }
        return true;
    }

    int RunWorker(const std::filesystem::path& worker, const WorkerJob& job, bool dryRun)
    {
#if defined(_WIN32)
        std::vector<std::wstring> values = {worker.native(),
                                            L"--source",
                                            job.source.native(),
                                            L"--output",
                                            job.output.native(),
                                            L"--sha256",
                                            std::wstring(job.sha256.begin(), job.sha256.end())};
        if (dryRun)
            values.emplace_back(L"--dry-run");
        std::vector<const wchar_t*> arguments;
        arguments.reserve(values.size() + 1);
        for (const auto& value : values)
            arguments.push_back(value.c_str());
        arguments.push_back(nullptr);
        const intptr_t result = ::_wspawnv(_P_WAIT, worker.c_str(), arguments.data());
        return result < 0 ? -1 : static_cast<int>(result);
#else
        const pid_t child = ::fork();
        if (child == 0)
        {
            std::vector<std::string> values = {worker.string(),     "--source", job.source.string(), "--output",
                                               job.output.string(), "--sha256", job.sha256};
            if (dryRun)
                values.emplace_back("--dry-run");
            std::vector<char*> arguments;
            arguments.reserve(values.size() + 1);
            for (auto& value : values)
                arguments.push_back(value.data());
            arguments.push_back(nullptr);
            ::execv(arguments.front(), arguments.data());
            ::_exit(127);
        }
        if (child < 0)
            return -1;
        int status = 0;
        while (::waitpid(child, &status, 0) < 0)
        {
            if (errno != EINTR)
                return -1;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
    }

    bool RunWorkerQueue(const std::filesystem::path& worker, std::vector<WorkerJob>& jobs, std::size_t concurrency,
                        bool dryRun, std::string& error)
    {
        std::vector<int> results(jobs.size(), -1);
        std::atomic<std::size_t> next{0};
        const std::size_t laneCount = std::min(concurrency, std::max<std::size_t>(jobs.size(), 1));
        std::vector<std::thread> lanes;
        lanes.reserve(laneCount);
        for (std::size_t lane = 0; lane < laneCount; ++lane)
        {
            lanes.emplace_back(
                [&]
                {
                    while (true)
                    {
                        const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= jobs.size())
                            return;
                        results[index] = RunWorker(worker, jobs[index], dryRun);
                    }
                });
        }
        for (auto& lane : lanes)
            lane.join();
        for (std::size_t index = 0; index < jobs.size(); ++index)
        {
            if (results[index] != 0)
            {
                error =
                    "worker job '" + jobs[index].portablePath + "' failed with exit " + std::to_string(results[index]);
                return false;
            }
        }
        return true;
    }

    bool ValidateWorkerOutputs(const std::filesystem::path& scratch, const std::vector<WorkerJob>& jobs,
                               std::string& error)
    {
        for (const auto& job : jobs)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(job.output, ec) || ec)
            {
                error = "worker did not create expected output '" + job.portablePath + "'";
                return false;
            }
            std::string actual;
            if (!Spark::AssetPipeline::ComputeFileSha256(job.output, actual, error) || actual != job.sha256)
            {
                if (error.empty())
                    error = "worker output digest mismatch for '" + job.portablePath + "'";
                return false;
            }
        }
        std::size_t outputCount = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator iterator(scratch, ec), end; !ec && iterator != end;
             iterator.increment(ec))
        {
            const auto status = iterator->symlink_status(ec);
            if (ec)
                break;
            if (std::filesystem::is_symlink(status) || IsLinkLike(iterator->path()))
            {
                error = "worker scratch output contains a symbolic link";
                return false;
            }
            if (std::filesystem::is_regular_file(status))
                ++outputCount;
        }
        if (ec || outputCount != jobs.size())
        {
            error = ec ? "failed to enumerate worker outputs: " + ec.message()
                       : "worker output set does not match the scheduled job set";
            return false;
        }
        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    Spark::AssetPipeline::CookRequest request;
    std::filesystem::path worker;
    std::unique_ptr<ScratchDirectory> workerScratch;
    std::size_t jobs = 1;
    bool jobsSpecified = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if ((argument == "--source" || argument == "--output" || argument == "--manifest" || argument == "--worker" ||
             argument == "--jobs") &&
            index + 1 < argc)
        {
            const std::string value = argv[++index];
            if (argument == "--source")
                request.sourceRoot = value;
            else if (argument == "--output")
                request.outputRoot = value;
            else if (argument == "--manifest")
                request.manifestPath = value;
            else if (argument == "--worker")
                worker = value;
            else
            {
                jobsSpecified = true;
                if (!ParseJobCount(value, jobs))
                {
                    std::cerr << "SparkCooker: --jobs must be an integer from 1 to 64\n";
                    return 2;
                }
            }
        }
        else if (argument == "--dry-run")
        {
            request.dryRun = true;
        }
        else if (argument == "--help" || argument == "-h")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            PrintUsage();
            return 2;
        }
    }
    if (request.sourceRoot.empty() || request.outputRoot.empty() || (jobsSpecified && worker.empty()))
    {
        PrintUsage();
        return 2;
    }
    if (!worker.empty())
    {
        std::error_code ec;
        worker = std::filesystem::absolute(worker, ec).lexically_normal();
        if (ec || !std::filesystem::is_regular_file(worker, ec))
        {
            std::cerr << "SparkCooker: worker executable does not exist or is not a regular file\n";
            return 2;
        }

        std::filesystem::path scratchPath;
        std::string workerError;
        if (!CreateScratchDirectory(scratchPath, workerError))
        {
            std::cerr << "SparkCooker: " << workerError << '\n';
            return 1;
        }
        workerScratch = std::make_unique<ScratchDirectory>(scratchPath);
        std::vector<WorkerJob> workerJobs;
        if (!BuildWorkerJobs(request.sourceRoot, workerScratch->Path(), workerJobs, workerError) ||
            !RunWorkerQueue(worker, workerJobs, jobs, request.dryRun, workerError) ||
            (!request.dryRun && !ValidateWorkerOutputs(workerScratch->Path(), workerJobs, workerError)))
        {
            std::cerr << "SparkCooker: " << workerError << '\n';
            return 1;
        }
        if (!request.dryRun)
            request.sourceRoot = workerScratch->Path();
    }

    if (request.manifestPath.empty())
        request.manifestPath = request.outputRoot / "spark-cook-manifest.json";
    request.onProgress = [](const Spark::AssetPipeline::CookRecord& record, std::size_t current, std::size_t total)
    {
        std::cout << '[' << current << '/' << total << "] " << (record.updated ? "cooked " : "unchanged ")
                  << record.path << '\n'
                  << std::flush;
    };

    const auto result = Spark::AssetPipeline::CookAssets(request);
    if (!result.Succeeded())
    {
        std::cerr << "SparkCooker: " << result.error << '\n';
        return 1;
    }
    std::cout << "SparkCooker: " << result.records.size() << " asset(s), " << result.updatedCount << " updated, "
              << result.unchangedCount << " unchanged\nmanifest-sha256 " << result.manifestSha256 << '\n';
    return 0;
}
