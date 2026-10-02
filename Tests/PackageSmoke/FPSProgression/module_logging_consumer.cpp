#include "module_logging_probe.h"
#include "module_logging_context.h"
#include <Spark/ModuleLog.h>

#include <Spark/IConsole.h>
#include <Spark/ILogger.h>

#include <stdexcept>
#include <string>
#include <vector>

class ProbeLogger final : public Spark::ILogger
{
  public:
    void Info(const char* message) override { m_info.emplace_back(message ? message : ""); }
    void Warn(const char*) override { ++m_warnCount; }
    void Error(const char*) override { ++m_errorCount; }
    void Debug(const char*) override { ++m_debugCount; }

    [[nodiscard]] std::size_t InfoCount() const { return m_info.size(); }
    [[nodiscard]] std::size_t WarnCount() const { return m_warnCount; }
    [[nodiscard]] std::size_t ErrorCount() const { return m_errorCount; }
    [[nodiscard]] std::size_t DebugCount() const { return m_debugCount; }

  private:
    std::vector<std::string> m_info;
    std::size_t m_warnCount = 0;
    std::size_t m_errorCount = 0;
    std::size_t m_debugCount = 0;
};

class ThrowingLogger final : public Spark::ILogger
{
  public:
    void Info(const char*) override { throw std::runtime_error("throwing logger"); }
    void Warn(const char*) override { throw std::runtime_error("throwing logger"); }
    void Error(const char*) override { throw std::runtime_error("throwing logger"); }
    void Debug(const char*) override { throw std::runtime_error("throwing logger"); }
};

int main()
{
    ProbeLogger loggerA;
    ProbeLogger loggerB;
    ProbeContext contextA(&loggerA);
    ProbeContext contextB(&loggerB);

    ProbeLogger hostLogger;
    ProbeContext hostContext(&hostLogger);
    Spark::ModuleLog::Bind(&hostContext);

    ModuleLogProbeBindA(&contextA);
    ModuleLogProbeBindB(&contextB);
    if (!ModuleLogProbeObserveA(&contextA) || !ModuleLogProbeObserveB(&contextB))
    {
        return 1;
    }
    if (loggerA.InfoCount() != 1 || loggerB.InfoCount() != 1)
    {
        return 2;
    }
    if (loggerA.WarnCount() != 1 || loggerA.ErrorCount() != 1 || loggerA.DebugCount() != 1 ||
        loggerB.WarnCount() != 1 || loggerB.ErrorCount() != 1 || loggerB.DebugCount() != 1)
    {
        return 3;
    }
    if (contextA.ConsolePrintCount() != 1 || contextB.ConsolePrintCount() != 1)
    {
        return 6;
    }

    if (Spark::ModuleLog::BoundContext() != &hostContext || hostLogger.InfoCount() != 0)
    {
        return 7;
    }

    // Unbinding image A must not clear image B's independent slot.
    ModuleLogProbeBindA(nullptr);
    if (!ModuleLogProbeObserveA(nullptr) || !ModuleLogProbeObserveB(&contextB) || loggerA.InfoCount() != 1 ||
        loggerB.InfoCount() != 2)
    {
        return 4;
    }

    ModuleLogProbeBindB(nullptr);

    // A host without a logger is a supported no-op for every logging helper.
    ProbeContext noLogger(nullptr);
    Spark::ModuleLog::Bind(&noLogger);
    Spark::ModuleLog::Info("no logger");
    Spark::ModuleLog::Warn("no logger");
    Spark::ModuleLog::Error("no logger");
    Spark::ModuleLog::Debug("no logger");

    // A logger exception is allowed to propagate; the SDK must not corrupt or
    // swallow host behavior while routing the call.
    ThrowingLogger throwingLogger;
    ProbeContext throwingContext(&throwingLogger);
    Spark::ModuleLog::Bind(&throwingContext);
    bool threw = false;
    try
    {
        Spark::ModuleLog::Info("throwing logger");
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    Spark::ModuleLog::Bind(nullptr);
    if (!threw)
    {
        return 5;
    }
    return 0;
}
