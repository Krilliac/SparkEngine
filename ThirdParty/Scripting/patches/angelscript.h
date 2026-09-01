#pragma once

// Keep the upstream submodule immutable while applying SparkEngine's bounded
// compatibility fix in the build and packaged SDK include path.
#if defined(SPARK_ANGELSCRIPT_BUILD_TREE)
#include "../angelscript-mirror/sdk/angelscript/include/angelscript.h"
#else
#include "angelscript_upstream/angelscript.h"
#endif

#include "AngelScriptPackedBytecode.h"
