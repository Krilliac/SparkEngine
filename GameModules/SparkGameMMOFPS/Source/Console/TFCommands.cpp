/**
 * @file TFCommands.cpp
 * @brief TERRAFRONT console command registration (tf_*).
 *
 * OWNERSHIP: shared registration point — each wave appends its commands here.
 * Keep one registration function; do not create parallel registrars.
 *
 * W0 stub: registers status/help only. Planned surface (DESIGN.md):
 *   tf_status, tf_host [port], tf_connect <ip[:port]>, tf_disconnect,
 *   tf_faction <mra|auc|hlx>, tf_spawn [region], tf_class <name>,
 *   tf_give <weapon>, tf_flux <n>, tf_capture <region>, tf_regions,
 *   tf_vehicle <drifter|aegis|ravager>, tf_reload_data, tf_bots <n>
 */

#include "Core/SparkGameMMOFPS.h"
#include "Utils/SparkConsole.h"

using namespace Terrafront;

void TerrafrontModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand(
        "tf_status",
        [this](const std::vector<std::string>&) -> std::string
        {
            const char* role =
                m_ctx.role == NetRole::Standalone      ? "standalone" :
                m_ctx.role == NetRole::ListenHost      ? "listen-host" :
                m_ctx.role == NetRole::DedicatedServer ? "dedicated"   : "client";
            return std::string("[TF] TERRAFRONT role=") + role +
                   " faction=" + FactionTag(m_ctx.localFaction);
        },
        "Show TERRAFRONT module status");
}
