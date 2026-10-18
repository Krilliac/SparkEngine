#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

int main(int argc, char* argv[])
{
    if (const char* capture = std::getenv("SPARK_FAKE_GH_CAPTURE"))
    {
        std::ofstream output(capture, std::ios::binary | std::ios::app);
        for (int index = 1; index < argc; ++index)
            output << argv[index] << '\n';
        if (const char* host = std::getenv("GH_HOST"))
            output << "gh-host=" << host << '\n';
#ifdef _WIN32
        if (const char* value = std::getenv("SPARK_FAKE_GH_SENTINEL_HANDLE"))
        {
            const auto number = std::strtoull(value, nullptr, 10);
            const bool signaled = SetEvent(reinterpret_cast<HANDLE>(number)) != FALSE;
            output << "sentinel-set=" << (signaled ? "yes" : "no") << '\n';
        }
#else
        if (const char* value = std::getenv("SPARK_FAKE_GH_SENTINEL_FD"))
        {
            const int descriptor = std::atoi(value);
            const bool inherited = write(descriptor, "X", 1) == 1;
            output << "sentinel-inherited=" << (inherited ? "yes" : "no") << '\n';
        }
#endif
        output << "--end-call--\n";
    }
    const std::string mode = std::getenv("SPARK_FAKE_GH_MODE") ? std::getenv("SPARK_FAKE_GH_MODE") : "success";
    if (mode == "timeout")
    {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return 0;
    }
    if (mode == "delayed-success")
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (mode == "failure")
        return 1;
    if (mode == "unconfirmed")
    {
        std::cout << "https://example.invalid/issues/1\n";
        return 0;
    }
    std::cout << "https://github.com/Krilliac/SparkEngine/issues/42\n";
    return 0;
}
