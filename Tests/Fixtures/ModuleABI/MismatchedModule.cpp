#include <Spark/IModule.h>
#include <Spark/ModuleABI.h>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    constexpr SparkModuleCompatibilityDescriptor MakeMismatchedDescriptor()
    {
        auto descriptor = Spark::kExpectedModuleCompatibility;
        ++descriptor.sdkVersion;
        return descriptor;
    }

    constexpr SparkModuleCompatibilityDescriptor kMismatchedDescriptor = MakeMismatchedDescriptor();

    void MarkUnexpectedExecution(const char* phase)
    {
        const char* sentinelPath = std::getenv("SPARK_MODULE_ABI_SENTINEL");
        if (!sentinelPath || sentinelPath[0] == '\0')
            return;

        if (FILE* sentinel = std::fopen(sentinelPath, "ab"))
        {
            std::fputs(phase, sentinel);
            std::fputc('\n', sentinel);
            std::fclose(sentinel);
        }
    }

    struct StaticConstructorSentinel
    {
        StaticConstructorSentinel() { MarkUnexpectedExecution("StaticConstructor"); }
    };

    StaticConstructorSentinel g_staticConstructorSentinel;
} // namespace

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        MarkUnexpectedExecution("DllMain");
    return TRUE;
}
#endif

extern "C" SPARK_MODULE_API const SparkModuleCompatibilityDescriptor* SparkGetModuleCompatibility()
{
    return &kMismatchedDescriptor;
}

extern "C" SPARK_MODULE_API void SparkModuleInjectConsole(void*)
{
    MarkUnexpectedExecution("SparkModuleInjectConsole");
}

extern "C" SPARK_MODULE_API void SparkModuleInjectEngineContext(void*)
{
    MarkUnexpectedExecution("SparkModuleInjectEngineContext");
}

extern "C" SPARK_MODULE_API void SparkModuleInjectImGui(void*, void*, void*, void*)
{
    MarkUnexpectedExecution("SparkModuleInjectImGui");
}

extern "C" SPARK_MODULE_API Spark::IModule* CreateModule()
{
    MarkUnexpectedExecution("CreateModule");
    return nullptr;
}

extern "C" SPARK_MODULE_API void DestroyModule(Spark::IModule*)
{
    MarkUnexpectedExecution("DestroyModule");
}
