#include <cstdint>

#if defined(_WIN32)
#define SPARK_TEST_DEPENDENCY_EXPORT __declspec(dllexport)
#else
#define SPARK_TEST_DEPENDENCY_EXPORT __attribute__((visibility("default")))
#endif

extern "C" SPARK_TEST_DEPENDENCY_EXPORT uint32_t SparkTestSiblingDependencyValue()
{
    return 0x51B11A7u;
}
