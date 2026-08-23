#include <Spark/ModuleRegistry.h>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
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

    class CompatibleModule final : public Spark::IModule
    {
      public:
        Spark::ModuleInfo GetModuleInfo() const override
        {
            Spark::ModuleInfo info{};
            info.name = "Spark Compatible ABI Fixture";
            info.version = "1.0.0";
            info.kind = Spark::ModuleKind::Addon;
            return info;
        }

        bool OnLoad(Spark::IEngineContext*) override
        {
            const char* failOnLoad = std::getenv("SPARK_MODULE_ABI_FAIL_ON_LOAD");
            return !failOnLoad || failOnLoad[0] == '\0';
        }
        void OnUnload() override {}
        void OnUpdate(float) override {}
    };
} // namespace

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        MarkUnexpectedExecution("DllMain");
    return TRUE;
}
#endif

SPARK_IMPLEMENT_MODULE(CompatibleModule)
