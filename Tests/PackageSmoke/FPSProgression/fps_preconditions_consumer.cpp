#include "Core/FPSAssert.h"
#include "module_logging_context.h"

#include <stdexcept>
#include <string_view>

#if defined(_WIN32)
#include <crtdbg.h>
#include <windows.h>
#else
#include <sys/resource.h>
#endif

class PreconditionLogger final : public Spark::ILogger
{
  public:
    explicit PreconditionLogger(bool shouldThrow) : m_shouldThrow(shouldThrow) {}
    void Info(const char*) override {}
    void Warn(const char*) override {}
    void Debug(const char*) override {}
    void Error(const char*) override
    {
        std::fputs("FPS_SDK_LOG_ATTEMPT\n", stderr);
        std::fflush(stderr);
        if (m_shouldThrow)
        {
            throw std::runtime_error("intentional logger failure");
        }
    }

  private:
    bool m_shouldThrow;
};

int main(int argc, char** argv)
{
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
    const rlimit noCoreDump{0, 0};
    if (setrlimit(RLIMIT_CORE, &noCoreDump) != 0)
    {
        return 2;
    }
#endif
    if (argc != 2)
    {
        return 2;
    }
    const std::string_view mode(argv[1]);
    if (mode == "false")
    {
        FPS_REQUIRE_MSG(false, "intentional precondition failure");
    }
    else if (mode == "null")
    {
        int* value = nullptr;
        FPS_REQUIRE_NOT_NULL(value);
    }
    else if (mode == "bound" || mode == "throwing")
    {
        PreconditionLogger logger(mode == "throwing");
        ProbeContext context(&logger);
        Spark::ModuleLog::Bind(&context);
        FPS_REQUIRE_MSG(false, "bound precondition failure");
    }
    else if (mode == "single")
    {
        int evaluations = 0;
        FPS_REQUIRE_MSG(++evaluations == 1, "single evaluation");
        int value = 0;
        FPS_REQUIRE_NOT_NULL((++evaluations, &value));
        return evaluations == 2 ? 0 : 1;
    }
    else
    {
        return 2;
    }
    return 0;
}
