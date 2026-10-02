/**
 * @file TFHandoffParticipant.h
 * @brief TERRAFRONT participant in the authenticated, fenced area-control handoff.
 *
 * Game-thread only: the host dispatcher marshals control-thread calls. Owned by TFServerSim;
 * database and authority references outlive it. Per-command strings and database snapshots allocate
 * on travel events, never during ordinary movement. Capacity is the module's declared player cap.
 */
#pragma once

#include "Engine/Networking/AreaHandoffParticipant.h"
#include "Net/TFHandoffState.h"
#include "Persistence/TFDatabase.h"

#ifdef ENABLE_NETWORKING
namespace Terrafront
{
    class TFHandoffParticipant final : public Spark::Net::IAreaHandoffParticipant
    {
      public:
        /// @brief Game-thread pawn operations supplied by the live authoritative simulation.
        class IAuthority
        {
          public:
            virtual ~IAuthority() = default;
            virtual bool ResolveContinent(Spark::Net::AreaID area, std::string& key) const = 0;
            virtual bool Capture(uint64_t character, TFHandoffState& state) = 0;
            virtual bool CanInstall(uint64_t character, const TFHandoffState& state) const = 0;
            virtual bool Suspend(uint64_t character) = 0;
            virtual bool Install(const TFCharacterRecord& character, const TFHandoffState& state) = 0;
            virtual void Retire(uint64_t character) = 0;
        };

        TFHandoffParticipant(TFDatabase& database, IAuthority& authority) : m_database(database), m_authority(authority)
        {
        }

        Spark::Net::HandoffResult Prepare(const Spark::Net::HandoffRequest& request) override;
        Spark::Net::HandoffResult Transfer(const Spark::Net::HandoffRequest& request) override;
        Spark::Net::HandoffResult Commit(const Spark::Net::HandoffRequest& request) override;
        Spark::Net::HandoffResult Acknowledge(const Spark::Net::HandoffRequest& request) override;
        Spark::Net::HandoffResult Abort(const Spark::Net::HandoffRequest& request) override;

        /// @brief Public routing identity; never a login credential or session token.
        static std::string SessionId(uint64_t character);

      private:
        struct Command
        {
            uint64_t character = 0;
            std::string operation;
            std::string source;
            std::string destination;
            bool isSource = false;
        };
        bool DecodeCommand(const Spark::Net::HandoffRequest& request, Command& command) const;
        bool ReadReservation(const Command& command, TFCharacterRecord& row, TFHandoffState& state) const;

        TFDatabase& m_database;
        IAuthority& m_authority;
    };
} // namespace Terrafront
#endif
