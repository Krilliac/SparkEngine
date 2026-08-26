#include <Spark/ModuleDllMain.h>

// Keep a translation-unit symbol on platforms where ModuleDllMain.h is a no-op.
extern "C" int SparkPackageModuleHeaderSmokeAnchor()
{
    return 0;
}
