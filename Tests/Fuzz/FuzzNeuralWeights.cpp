/**
 * @file FuzzNeuralWeights.cpp
 * @brief Production-entry-point libFuzzer harness for the .nnw file loader.
 */

#include "Graphics/Neural/NeuralWeights.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 8;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 4096;

static_assert(SPARK_FUZZ_MAX_DEPTH == Spark::Graphics::Neural::kMaxNetworkLayers);

namespace
{
    [[noreturn]] void InfrastructureFailure(const char* operation)
    {
        std::fprintf(stderr, "SparkFuzzNeuralWeights infrastructure failure during %s: %s\n", operation,
                     std::strerror(errno));
        std::_Exit(70);
    }

    class TemporaryInput
    {
      public:
        TemporaryInput()
        {
            std::array<char, 64> pattern{};
            std::snprintf(pattern.data(), pattern.size(), "/tmp/spark-nnw-fuzz-XXXXXX");
            m_descriptor = ::mkstemp(pattern.data());
            if (m_descriptor < 0)
                InfrastructureFailure("mkstemp");
            m_path = pattern.data();
        }

        ~TemporaryInput()
        {
            if (m_descriptor >= 0)
                ::close(m_descriptor);
            if (!m_path.empty())
                ::unlink(m_path.c_str());
        }

        void Write(const uint8_t* data, size_t size)
        {
            size_t offset = 0;
            while (offset < size)
            {
                const ssize_t written = ::write(m_descriptor, data + offset, size - offset);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    InfrastructureFailure("write");
                offset += static_cast<size_t>(written);
            }
            if (::close(m_descriptor) != 0)
                InfrastructureFailure("close");
            m_descriptor = -1;
        }

        const std::string& Path() const { return m_path; }

      private:
        int m_descriptor = -1;
        std::string m_path;
    };
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > SPARK_FUZZ_MAX_INPUT_BYTES)
    {
        return 0;
    }
    if (data == nullptr && size != 0)
    {
        return 0;
    }

    TemporaryInput input;
    input.Write(data, size);
    (void)Spark::Graphics::Neural::LoadWeights(input.Path());
    return 0;
}
