#include "module_logging_probe.h"

#include <Spark/ModuleLog.h>

extern "C" bool ModuleLogProbeObserveA(Spark::IEngineContext* expected)
{
    const bool isolated = Spark::ModuleLog::BoundContext() == expected;
    Spark::ModuleLog::Info("module A");
    Spark::ModuleLog::Warn("module A warning");
    Spark::ModuleLog::Error("module A error");
    Spark::ModuleLog::Debug("module A debug");
    Spark::ModuleLog::Print("module A print", "INFO");
    return isolated;
}
