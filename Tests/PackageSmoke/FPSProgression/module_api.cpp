#include "Core/SparkGameFPS.h"

#include <Spark/Version.h>

#include <type_traits>

static_assert(std::is_base_of_v<Spark::IModule, SparkGameModule>);
static_assert(!std::is_abstract_v<SparkGameModule>);

int main()
{
    return Spark::IsSDKCompatible(SPARK_SDK_VERSION) ? 0 : 1;
}
