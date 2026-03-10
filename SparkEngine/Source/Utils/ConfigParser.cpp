/**
 * @file ConfigParser.cpp
 * @brief Out-of-line ConfigParser methods that depend on LocalFileCache
 */

#include "ConfigParser.h"
#include "LocalFileCache.h"

namespace Spark
{

    bool ConfigParser::Load(const std::string& path, LocalFileCache& cache)
    {
        auto result = cache.ReadText(path);
        if (result.IsErr())
            return false;
        return LoadFromString(result.Value());
    }

    bool ConfigParser::Save(const std::string& path, LocalFileCache& cache) const
    {
        std::string content = SaveToString();
        auto result = cache.WriteText(path, content);
        return result.IsOk();
    }

} // namespace Spark
