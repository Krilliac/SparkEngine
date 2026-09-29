/**
 * @file IConsole.h
 * @brief Console command registration game modules reach through IEngineContext::GetConsole()
 *
 * The host implements IConsole (EngineSdkConsole) on its own console command
 * registry, the one SparkConsole.exe and the in-game console dispatch into. A
 * module registers its commands through the context it received in OnLoad
 * instead of including the engine-private Utils/SparkConsole.h.
 *
 * ## Usage
 * @code
 *   #include <Spark/IConsole.h>
 *
 *   bool MyModule::OnLoad(Spark::IEngineContext* context)
 *   {
 *       if (Spark::IConsole* console = context->GetConsole())
 *       {
 *           console->RegisterCommand(
 *               "my_status", [this](const std::vector<std::string>&) { return Describe(); },
 *               "Show my module's status", "MyModule", "my_status");
 *       }
 *       return true;
 *   }
 *
 *   void MyModule::OnUnload()
 *   {
 *       if (Spark::IConsole* console = m_context ? m_context->GetConsole() : nullptr)
 *           console->UnregisterCommand("my_status");
 *   }
 * @endcode
 *
 * Contract:
 * - Thread affinity: game thread. Register and unregister from OnLoad/OnUnload
 *   (or other game-thread callbacks); handlers run on the thread that executes
 *   console commands, which is the game thread.
 * - Ownership: host-owned; the pointer stays valid until after the module's
 *   OnUnload returns. Do not cache it past OnUnload.
 * - Handlers are std::functions whose code lives in the module image. A module
 *   must unregister every command it registered in OnUnload, before its image
 *   is unmapped. The host also removes a module's commands by owner on unload
 *   as a backstop, but that does not excuse the module.
 * - ABI: std::function, std::string and std::vector cross the boundary. That is
 *   sound because the SDK ABI is exact-match (IsSDKCompatible) with the same
 *   toolchain and CRT on both sides (OD-02).
 */

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    /**
     * @brief Abstract console command registry for game modules
     *
     * Implemented by the host; obtained from IEngineContext::GetConsole().
     */
    class IConsole
    {
      public:
        /** @brief A command handler: receives the arguments after the command name, returns the reply text. */
        using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

        virtual ~IConsole() = default;

        /**
         * @brief Register a console command
         * @param name Command name as typed in the console
         * @param handler Called with the command's arguments; its return value is the command output
         * @param help One-line description shown by the console's help
         * @param category Help grouping, usually the module name
         * @param usage Usage string shown by the console's help
         * @return true when this handler is now the registered one; false when the
         *         name or handler is empty, the host console is not running, or the
         *         host refuses the name because a different registrant (such as the
         *         engine itself) owns it
         */
        virtual bool RegisterCommand(std::string_view name, CommandHandler handler, std::string_view help,
                                     std::string_view category, std::string_view usage) = 0;

        /**
         * @brief Remove a command by name; unknown names are ignored
         *
         * Pass only names this module registered. While the host runs a module's
         * OnLoad/OnUnload it attributes the call to that module, and a command
         * owned by someone else is then left alone.
         */
        virtual void UnregisterCommand(std::string_view name) = 0;
    };

} // namespace Spark
