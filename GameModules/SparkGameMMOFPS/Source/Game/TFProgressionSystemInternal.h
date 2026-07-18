/**
 * @file TFProgressionSystemInternal.h
 * @brief Shared internals for the TFProgressionSystem*.cpp split parts: the
 *        whole-file text reader used by both the JSON persistence part and
 *        the suits.json table parse. Include only from the TFProgressionSystem
 *        translation units.
 */
#pragma once

#include <fstream>
#include <sstream>
#include <string>

namespace Terrafront
{
    namespace ProgressionDetail
    {

        /// Read a whole file into a string; false if it does not exist / can't open.
        inline bool ReadAllText(const char* path, std::string& out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
                return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        }

    } // namespace ProgressionDetail
} // namespace Terrafront
