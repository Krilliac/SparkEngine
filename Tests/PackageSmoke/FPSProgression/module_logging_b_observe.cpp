#include "module_logging_probe.h"

#include <Spark/ModuleLog.h>

extern "C" bool ModuleLogProbeObserveB(Spark::IEngineContext* expected)
{
    const bool isolated = Spark::ModuleLog::BoundContext() == expected;
    Spark::ModuleLog::Info("module B");
    Spark::ModuleLog::Warn("module B warning");
    Spark::ModuleLog::Error("module B error");
    Spark::ModuleLog::Debug("module B debug");
    Spark::ModuleLog::Print("module B print", "INFO");
    return isolated;
}
