/**
 * @file FuzzReplayMain.cpp
 * @brief Replays one committed fuzz corpus through a production harness without libFuzzer.
 *
 * The libFuzzer targets build only on Linux Clang, so this driver stands in for
 * libFuzzer's corpus replay on every compiler the test suite builds with (MSVC,
 * GCC, Clang). It links against a harness's LLVMFuzzerTestOneInput, feeds it
 * every regular file under the corpus directory once, in sorted order, and
 * fails when an input aborts or crashes the process, runs past the per-input
 * time limit, or when the corpus holds no seeds at all.
 *
 * Usage: SparkFuzzReplay<Parser> [--timeout-ms <ms>] <corpus-directory>
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace
{
    constexpr long long kDefaultTimeoutMs = 5000;
    constexpr long long kMaxTimeoutMs = 600000;

    // The deadline of the input currently executing (0 between inputs) and its
    // index into the sorted input list, read by the watchdog.
    std::atomic<long long> g_deadlineMs{0};
    std::atomic<std::size_t> g_currentIndex{0};
    std::atomic<bool> g_done{false};

    long long NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    int Usage()
    {
        std::fputs("usage: SparkFuzzReplay [--timeout-ms <1..600000>] <corpus-directory>\n", stderr);
        return 2;
    }

    // A hung input never returns to main, so the watchdog ends the process.
    void Watchdog(const std::vector<std::filesystem::path>& inputs)
    {
        while (!g_done.load(std::memory_order_acquire))
        {
            const long long deadline = g_deadlineMs.load(std::memory_order_acquire);
            if (deadline != 0 && NowMs() > deadline)
            {
                const std::size_t index = g_currentIndex.load(std::memory_order_acquire);
                std::fprintf(stderr, "FuzzReplay: TIMEOUT replaying %s\n", inputs[index].string().c_str());
                std::fflush(stderr);
                std::_Exit(124);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
} // namespace

int main(int argc, char* argv[])
{
#ifdef _WIN32
    // An abort or access violation must end the replay with a failing exit
    // code, never a modal error dialog that stalls CTest until its timeout.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    long long timeoutMs = kDefaultTimeoutMs;
    std::filesystem::path corpus;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--timeout-ms" && index + 1 < argc)
        {
            char* end = nullptr;
            timeoutMs = std::strtoll(argv[++index], &end, 10);
            if (end == nullptr || *end != '\0' || timeoutMs < 1 || timeoutMs > kMaxTimeoutMs)
                return Usage();
        }
        else if (corpus.empty() && !argument.starts_with("-"))
            corpus = std::filesystem::path(argument);
        else
            return Usage();
    }
    if (corpus.empty())
        return Usage();

    std::error_code error;
    std::vector<std::filesystem::path> inputs;
    for (std::filesystem::recursive_directory_iterator it(corpus, error), end; !error && it != end; it.increment(error))
    {
        if (it->is_regular_file(error))
            inputs.push_back(it->path());
    }
    if (error)
    {
        std::fprintf(stderr, "FuzzReplay: cannot enumerate corpus %s: %s\n", corpus.string().c_str(),
                     error.message().c_str());
        return 2;
    }
    // A replay that reads nothing passes vacuously; an empty corpus is a failure.
    if (inputs.empty())
    {
        std::fprintf(stderr, "FuzzReplay: corpus %s holds no seeds\n", corpus.string().c_str());
        return 2;
    }
    std::sort(inputs.begin(), inputs.end());

    std::thread watchdog(Watchdog, std::cref(inputs));
    int status = 0;
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        const std::filesystem::path& input = inputs[index];
        std::ifstream stream(input, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        if (!stream.eof() && stream.fail())
        {
            std::fprintf(stderr, "FuzzReplay: cannot read %s\n", input.string().c_str());
            status = 2;
            break;
        }

        // Printed before the call so an abort or crash names the input that caused it.
        std::printf("FuzzReplay: %s (%zu bytes)\n", input.string().c_str(), bytes.size());
        std::fflush(stdout);
        g_currentIndex.store(index, std::memory_order_release);
        g_deadlineMs.store(NowMs() + timeoutMs, std::memory_order_release);
        (void)LLVMFuzzerTestOneInput(bytes.empty() ? nullptr : reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                     bytes.size());
        g_deadlineMs.store(0, std::memory_order_release);
    }
    g_done.store(true, std::memory_order_release);
    watchdog.join();

    if (status == 0)
        std::printf("FuzzReplay: replayed %zu input(s) from %s\n", inputs.size(), corpus.string().c_str());
    return status;
}
