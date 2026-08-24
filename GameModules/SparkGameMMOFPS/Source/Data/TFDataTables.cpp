/**
 * @file TFDataTables.cpp
 * @brief JSON data-table loaders (factions, weapons, classes, vehicles,
 *        regions, presentation, and deployables).
 *
 * Loads the seven core tables from Assets/MMOFPS/Data/, validates them (unique ids,
 * known slot/kind/tier strings, conduit symmetry, complete initial ownership)
 * and fails LOUD: any error aborts the load with a clear log message and no
 * half-loaded state. Parsing goes through Spark::Json (JsonUtils.h), which is
 * backed by nlohmann/json when SPARK_HAS_NLOHMANN_JSON is available. This TU
 * owns file I/O, continent selection and the TFDataTables members; the
 * per-table parsers live in TFDataTablesParseRoster.cpp and
 * TFDataTablesParseWorld.cpp with shared helpers in TFDataTablesInternal.h.
 */
#include "Data/TFDataTables.h"
#include "Data/TFDataTablesInternal.h"
#include "Utils/LogMacros.h"
#include "Utils/JsonUtils.h"
#include "Utils/ConsoleVariable.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace Terrafront
{

    using namespace DataTablesDetail;

    namespace
    {

        constexpr const char* kDataDir = "Assets/MMOFPS/Data/";

        using Spark::Json::Value;

        bool ReadFileText(const std::string& path, std::string& out, std::string& err)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f.is_open())
            {
                err = "cannot open '" + path + "'";
                return false;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            if (out.empty())
            {
                err = "'" + path + "' is empty";
                return false;
            }
            return true;
        }

        /// Load + parse one JSON file; false (with err) on any failure.
        bool LoadJsonFile(const std::string& file, Value& out, std::string& err)
        {
            std::string text;
            if (!ReadFileText(std::string(kDataDir) + file, text, err))
                return false;
            out = Spark::Json::Parse(text);
            if (!out.IsObject())
            {
                err = file + ": malformed JSON (parse returned non-object)";
                return false;
            }
            return true;
        }

        // ------------------------------------------------------------------
        // W12 continent-2-data: which continent this server process loads.
        // The choice is PINNED at first load (function-local static in
        // LoadAllInternal): TFWorldSetup builds scene+collision once at module
        // init and does NOT rebuild on EvDataReloaded, so switching continents
        // mid-process would desync lattice vs. world. One continent per
        // server process. Boot-time selection: set the TF_CONTINENT
        // environment variable before launching (works today), or the
        // tf_continent cvar once a config/autoexec path can set cvars
        // pre-module-init (the cvar is read first so that path needs no
        // further loader changes).
        // ------------------------------------------------------------------
        Spark::CVar<std::string> cv_tfContinent(
            "tf_continent", "cindral_wastes", Spark::CVarFlags::RequiresRestart,
            "Continent (continents.json key) this server process loads at boot; restart required to change");

        /// continents.json key -> region-lattice data file ("regions.json" for
        /// the default). Never fails: unknown keys fall back to the default
        /// with a loud log so a typo cannot boot a half-configured server.
        struct ContinentSelection
        {
            std::string key;
            std::string regionsFile;
        };

        ContinentSelection ResolveContinentSelection()
        {
            std::string want = cv_tfContinent.Get();
            if (want == "cindral_wastes" || want.empty())
            {
                if (const char* env = std::getenv("TF_CONTINENT"); env && env[0] != '\0')
                    want = env;
            }
            if (want == "cindral_wastes" || want.empty())
                return {"cindral_wastes", "regions.json"};

            Value root;
            std::string err;
            if (LoadJsonFile("continents.json", root, err) && root["continents"].IsArray())
            {
                const Value& arr = root["continents"];
                for (size_t i = 0; i < arr.Size(); ++i)
                {
                    const Value& o = arr[i];
                    if (!o.IsObject() || GetStr(o, "key") != want)
                        continue;
                    const std::string file = GetStr(o, "regions");
                    if (!file.empty())
                    {
                        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] tf_continent=%s -> %s", want.c_str(),
                                       file.c_str());
                        return {want, file};
                    }
                }
            }
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] tf_continent '%s' unknown in continents.json (or entry lacks 'regions'); "
                            "falling back to Cindral Wastes",
                            want.c_str());
            return {"cindral_wastes", "regions.json"};
        }

    } // namespace

    // ===========================================================================

    TFDataTables::TFDataTables() = default;
    TFDataTables::~TFDataTables()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFDataTables::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;

        std::string err;
        if (!LoadAllInternal(err))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] Data table load FAILED: %s", err.c_str());
            return false; // fail loud - no half-loaded state
        }
        SPARK_LOG_INFO(
            Spark::LogCategory::Game,
            "[TF] Data tables loaded: %zu factions, %zu weapons, %zu classes, %zu vehicles, %zu regions (%s)",
            m_factions.size(), m_weapons.size(), m_classes.size(), m_vehicles.size(), m_continent.regions.size(),
            m_continent.name.c_str());
        return true;
    }

    void TFDataTables::Update(float deltaTime)
    {
        (void)deltaTime;
    }
    void TFDataTables::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFDataTables::Shutdown()
    {
        m_factions.clear();
        m_weapons.clear();
        m_classes.clear();
        m_vehicles.clear();
        m_continent = {};
        m_presentation = {};
        m_deployableVisuals.clear();
        m_loaded = false;
        m_initialized = false;
    }

    bool TFDataTables::ReloadAll()
    {
        std::string err;
        if (!LoadAllInternal(err))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] tf_reload_data FAILED (previous tables kept): %s",
                            err.c_str());
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Data tables reloaded from disk");
        if (m_events)
            m_events->Fire(EvDataReloaded{});
        return true;
    }

    bool TFDataTables::LoadAllInternal(std::string& outError)
    {
        // Parse everything into locals first; commit only if ALL tables validate,
        // so a bad reload never leaves partially updated state.
        Value jFactions, jWeapons, jClasses, jVehicles, jRegions, jPresentation, jDeployables;
        if (!LoadJsonFile("factions.json", jFactions, outError))
            return false;
        if (!LoadJsonFile("weapons.json", jWeapons, outError))
            return false;
        if (!LoadJsonFile("classes.json", jClasses, outError))
            return false;
        if (!LoadJsonFile("vehicles.json", jVehicles, outError))
            return false;
        // W12: region lattice selected by tf_continent/TF_CONTINENT, pinned for
        // the process lifetime; ReloadAll (tf_reload_data) re-reads the SAME
        // file — switching continents requires a server restart.
        static const ContinentSelection kContinent = ResolveContinentSelection();
        if (!LoadJsonFile(kContinent.regionsFile, jRegions, outError))
            return false;
        if (!LoadJsonFile("presentation.json", jPresentation, outError))
            return false;
        if (!LoadJsonFile("deployables.json", jDeployables, outError))
            return false;

        std::vector<FactionDef> factions;
        std::vector<WeaponDef> weapons;
        std::vector<ClassDef> classes;
        std::vector<VehicleDef> vehicles;
        ContinentDef continent;
        WorldPresentationDef presentation;
        std::vector<DeployableVisualDef> deployableVisuals;

        if (!ParseFactions(jFactions, factions, outError))
            return false;
        if (!ParseWeapons(jWeapons, weapons, outError))
            return false;
        if (!ParseClasses(jClasses, classes, outError))
            return false;
        if (!ParseVehicles(jVehicles, vehicles, outError))
            return false;
        if (!ParseRegions(jRegions, continent, outError))
            return false;
        continent.key = kContinent.key;
        if (!ParsePresentation(jPresentation, presentation, outError))
            return false;
        if (!ParseDeployables(jDeployables, deployableVisuals, outError))
            return false;

        // Cross-table check: vehicle seat weapons must reference known weapon keys.
        for (const VehicleDef& v : vehicles)
        {
            for (const VehicleSeatDef& s : v.seats)
            {
                if (s.weaponKey.empty())
                    continue;
                bool found = std::any_of(weapons.begin(), weapons.end(),
                                         [&](const WeaponDef& w) { return w.key == s.weaponKey; });
                if (!found)
                {
                    outError = "vehicles.json: '" + v.name + "' seat weapon '" + s.weaponKey + "' not in weapons.json";
                    return false;
                }
            }
        }

        m_factions = std::move(factions);
        m_weapons = std::move(weapons);
        m_classes = std::move(classes);
        m_vehicles = std::move(vehicles);
        m_continent = std::move(continent);
        m_presentation = std::move(presentation);
        m_deployableVisuals = std::move(deployableVisuals);
        m_loaded = true;
        return true;
    }

    // --- typed accessors --------------------------------------------------------

    const FactionDef* TFDataTables::GetFaction(FactionId f) const
    {
        for (const FactionDef& d : m_factions)
            if (d.id == f)
                return &d;
        return nullptr;
    }

    const WeaponDef* TFDataTables::GetWeapon(WeaponId id) const
    {
        for (const WeaponDef& w : m_weapons)
            if (w.id == id)
                return &w;
        return nullptr;
    }

    const WeaponDef* TFDataTables::GetWeaponByKey(const std::string& key) const
    {
        for (const WeaponDef& w : m_weapons)
            if (w.key == key)
                return &w;
        return nullptr;
    }

    const ClassDef* TFDataTables::GetClass(ClassId id) const
    {
        for (const ClassDef& c : m_classes)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    const ClassDef* TFDataTables::GetClassByName(const std::string& name) const
    {
        for (const ClassDef& c : m_classes)
            if (c.name == name)
                return &c;
        return nullptr;
    }

    const VehicleDef* TFDataTables::GetVehicle(VehicleId id) const
    {
        for (const VehicleDef& v : m_vehicles)
            if (v.id == id)
                return &v;
        return nullptr;
    }

    const DeployableVisualDef* TFDataTables::GetDeployableVisual(DeployableKind kind) const
    {
        for (const DeployableVisualDef& d : m_deployableVisuals)
            if (d.id == kind)
                return &d;
        return nullptr;
    }

    const RegionDef* TFDataTables::GetRegion(RegionId id) const
    {
        // ids are validated contiguous [0..N) at load time
        if (id >= m_continent.regions.size())
            return nullptr;
        return &m_continent.regions[id];
    }

    WeaponDef TFDataTables::ResolveWeapon(WeaponId id, FactionId f) const
    {
        const WeaponDef* base = GetWeapon(id);
        if (!base)
            return {};
        WeaponDef w = *base;
        if (const FactionDef* fd = GetFaction(f))
        {
            w.rofRpm *= fd->rofMult;
            w.damage *= fd->damageMult;
            w.minDamage *= fd->damageMult;
            w.reloadSec *= fd->reloadMult;
            w.gravity *= fd->projGravityMult;
        }
        return w;
    }

    void TFDataTables::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Data Tables"))
            return;
        ImGui::Text("Loaded: %s", m_loaded ? "yes" : "NO");
        ImGui::Text("Continent: %s (%.0fm, %zu regions)", m_continent.name.c_str(), m_continent.sizeM,
                    m_continent.regions.size());
        ImGui::Text("Weapons: %zu | Classes: %zu | Vehicles: %zu | Factions: %zu", m_weapons.size(), m_classes.size(),
                    m_vehicles.size(), m_factions.size());
        if (ImGui::Button("Reload (tf_reload_data)"))
            ReloadAll();
#endif
    }

} // namespace Terrafront
