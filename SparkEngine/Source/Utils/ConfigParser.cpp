/**
 * @file ConfigParser.cpp
 * @brief Out-of-line ConfigParser methods that depend on LocalFileCache
 */

#include "ConfigParser.h"
#include "LocalFileCache.h"
#include "Validate.h"

namespace Spark
{

    bool ConfigParser::Load(const std::string& path, LocalFileCache& cache)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Core, !path.empty(), false);
        auto result = cache.ReadText(path);
        if (result.IsErr())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "ConfigParser::Load failed to read '%s'", path.c_str());
            return false;
        }
        return LoadFromString(result.Value());
    }

    bool ConfigParser::Save(const std::string& path, LocalFileCache& cache) const
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Core, !path.empty(), false);
        std::string content = SaveToString();
        auto result = cache.WriteText(path, content);
        return result.IsOk();
    }

} // namespace Spark
