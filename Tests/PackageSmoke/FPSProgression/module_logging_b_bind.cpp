#include "module_logging_probe.h"

#include <Spark/ModuleLog.h>

extern "C" void ModuleLogProbeBindB(Spark::IEngineContext* context)
{
    Spark::ModuleLog::Bind(context);
}
