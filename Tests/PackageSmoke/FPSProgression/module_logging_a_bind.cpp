#include "module_logging_probe.h"

#include <Spark/ModuleLog.h>

extern "C" void ModuleLogProbeBindA(Spark::IEngineContext* context)
{
    Spark::ModuleLog::Bind(context);
}
