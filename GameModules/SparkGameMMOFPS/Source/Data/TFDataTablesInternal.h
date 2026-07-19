/**
 * @file TFDataTablesInternal.h
 * @brief Shared internals for the TFDataTables*.cpp split parts: the
 *        missing-key-tolerant JSON accessors, the faction-tag / one-of string
 *        helpers used by more than one parser, and the per-table parser
 *        declarations (defined in TFDataTablesParseRoster.cpp and
 *        TFDataTablesParseWorld.cpp, called from TFDataTables.cpp). Include
 *        only from the TFDataTables translation units.
 */
#pragma once

#include "Data/TFDataTables.h"
#include "Utils/JsonUtils.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace Terrafront
{
    namespace DataTablesDetail
    {

        using Spark::Json::Value;

        // Missing-key-tolerant accessors. Value::operator[] logs a warning on type
        // mismatch, so we pre-check HasKey to keep absent optional fields silent.
        inline float GetNum(const Value& o, const char* k, float def)
        {
            return o.HasKey(k) ? static_cast<float>(o[k].AsNumber(def)) : def;
        }
        inline int GetInt(const Value& o, const char* k, int def)
        {
            return o.HasKey(k) ? o[k].AsInt(def) : def;
        }
        inline bool GetBool(const Value& o, const char* k, bool def)
        {
            return o.HasKey(k) ? o[k].AsBool(def) : def;
        }
        inline std::string GetStr(const Value& o, const char* k, const std::string& def = {})
        {
            return o.HasKey(k) ? o[k].AsString(def) : def;
        }

        inline bool ParseFactionTag(const std::string& tag, FactionId& out)
        {
            if (tag == "MRA")
                out = FactionId::MRA;
            else if (tag == "AUC")
                out = FactionId::AUC;
            else if (tag == "HLX")
                out = FactionId::HLX;
            else if (tag == "ALL" || tag.empty() || tag == "None")
                out = FactionId::None;
            else
                return false;
            return true;
        }

        inline bool IsOneOf(const std::string& s, std::initializer_list<const char*> opts)
        {
            for (const char* o : opts)
                if (s == o)
                    return true;
            return false;
        }

        // Per-table parsers. Roster tables (factions/weapons/classes/vehicles)
        // are defined in TFDataTablesParseRoster.cpp; world tables
        // (regions/presentation/deployables) in TFDataTablesParseWorld.cpp.
        bool ParseFactions(const Value& root, std::vector<FactionDef>& out, std::string& err);
        bool ParseWeapons(const Value& root, std::vector<WeaponDef>& out, std::string& err);
        bool ParseClasses(const Value& root, std::vector<ClassDef>& out, std::string& err);
        bool ParseVehicles(const Value& root, std::vector<VehicleDef>& out, std::string& err);
        bool ParseRegions(const Value& root, ContinentDef& out, std::string& err);
        bool ParsePresentation(const Value& root, WorldPresentationDef& out, std::string& err);
        bool ParseDeployables(const Value& root, std::vector<DeployableVisualDef>& out, std::string& err);

    } // namespace DataTablesDetail
} // namespace Terrafront
