/** @file TFHandoffParticipant.cpp @brief Durable ownership transitions for TERRAFRONT area control. */
#include "Net/TFHandoffParticipant.h"

#include <charconv>

#ifdef ENABLE_NETWORKING
namespace Terrafront
{
    using Spark::Net::HandoffRequest;
    using Spark::Net::HandoffResult;

    std::string TFHandoffParticipant::SessionId(uint64_t character)
    {
        return "tf/" + std::to_string(character);
    }

    bool TFHandoffParticipant::DecodeCommand(const HandoffRequest& request, Command& command) const
    {
        if (!m_database.IsOpen() || request.epoch == 0 || !request.sessionId.starts_with("tf/") ||
            request.sourceArea == request.targetArea)
        {
            return false;
        }
        const std::string_view number(request.sessionId.data() + 3, request.sessionId.size() - 3);
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), command.character);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || command.character == 0 ||
            SessionId(command.character) != request.sessionId ||
            !m_authority.ResolveContinent(request.sourceArea, command.source) ||
            !m_authority.ResolveContinent(request.targetArea, command.destination) ||
            command.source == command.destination)
        {
            return false;
        }
        command.isSource = m_database.BoundContinent() == command.source;
        if (!command.isSource && m_database.BoundContinent() != command.destination)
        {
            return false;
        }
        command.operation = request.sessionId + "/" + std::to_string(request.epoch);
        return true;
    }

    bool TFHandoffParticipant::ReadReservation(const Command& command, TFCharacterRecord& row,
                                               TFHandoffState& state) const
    {
        return m_database.FindCharacter(command.character, row) && row.migrationOperation == command.operation &&
               row.residentContinent == command.source && row.migrationSource == command.source &&
               row.migrationDestination == command.destination && TFHandoffState::Decode(row.migrationPayload, state);
    }

    HandoffResult TFHandoffParticipant::Prepare(const HandoffRequest& request)
    {
        if (!m_database.IsOpen() || m_database.BoundContinent().empty())
        {
            return HandoffResult::Unavailable;
        }
        Command command;
        if (!DecodeCommand(request, command))
        {
            return HandoffResult::Rejected;
        }
        TFCharacterRecord row;
        TFHandoffState state;
        if (!command.isSource)
        {
            return ReadReservation(command, row, state) && m_authority.CanInstall(command.character, state)
                       ? HandoffResult::Applied
                       : HandoffResult::Unavailable;
        }
        if (ReadReservation(command, row, state))
        {
            return m_authority.Suspend(command.character) ? HandoffResult::Duplicate : HandoffResult::Unavailable;
        }
        if (!m_authority.Capture(command.character, state))
        {
            return HandoffResult::Rejected;
        }
        const std::string payload = state.Encode();
        if (payload.empty() || !m_database.ReserveMigration(command.character, command.operation, request.epoch,
                                                            command.destination, payload, row))
        {
            return HandoffResult::Unavailable;
        }
        return m_authority.Suspend(command.character) ? HandoffResult::Applied : HandoffResult::Unavailable;
    }

    HandoffResult TFHandoffParticipant::Transfer(const HandoffRequest& request)
    {
        Command command;
        TFCharacterRecord row;
        TFHandoffState state;
        if (!DecodeCommand(request, command) || !ReadReservation(command, row, state))
        {
            return HandoffResult::Rejected;
        }
        return command.isSource || m_authority.CanInstall(command.character, state) ? HandoffResult::Applied
                                                                                    : HandoffResult::Unavailable;
    }

    HandoffResult TFHandoffParticipant::Commit(const HandoffRequest& request)
    {
        Command command;
        TFCharacterRecord row;
        TFHandoffState state;
        if (!DecodeCommand(request, command) || !m_database.FindCharacter(command.character, row))
        {
            return HandoffResult::Rejected;
        }
        const bool completed = row.migrationLastOperation == command.operation && row.migrationOperation.empty() &&
                               row.residentContinent == command.destination;
        if (!completed && !ReadReservation(command, row, state))
        {
            return HandoffResult::Rejected;
        }
        if (command.isSource)
        {
            if (!m_authority.Suspend(command.character))
            {
                return HandoffResult::Unavailable;
            }
            return completed ? HandoffResult::Duplicate : HandoffResult::Applied;
        }
        if (!TFHandoffState::Decode(row.migrationPayload, state) || !m_authority.CanInstall(command.character, state))
        {
            return HandoffResult::Unavailable;
        }
        if (!m_database.CommitMigration(command.character, command.operation, row) || !m_authority.Install(row, state))
        {
            return HandoffResult::Unavailable;
        }
        return completed ? HandoffResult::Duplicate : HandoffResult::Applied;
    }

    HandoffResult TFHandoffParticipant::Acknowledge(const HandoffRequest& request)
    {
        Command command;
        TFCharacterRecord row;
        if (!DecodeCommand(request, command) || !m_database.FindCharacter(command.character, row))
        {
            return HandoffResult::Unavailable;
        }
        if (row.residentContinent != command.destination || row.migrationLastOperation != command.operation ||
            !row.migrationOperation.empty())
        {
            return HandoffResult::Rejected;
        }
        if (command.isSource)
        {
            m_authority.Retire(command.character);
        }
        return HandoffResult::Applied;
    }

    HandoffResult TFHandoffParticipant::Abort(const HandoffRequest& request)
    {
        Command command;
        TFCharacterRecord row;
        TFHandoffState state;
        if (!DecodeCommand(request, command) || !m_database.FindCharacter(command.character, row))
        {
            return HandoffResult::Rejected;
        }
        const bool aborted = row.migrationState == "rolled_back" && row.migrationLastOperation == command.operation &&
                             row.migrationEpoch == request.epoch && row.migrationSource == command.source &&
                             row.migrationDestination == command.destination;
        if (aborted && row.residentContinent.empty())
        {
            // Abort may have finished logout after a disconnected source (or its startup recovery).
            // A lost reply must not resurrect that pawn or strand the gateway in its abort phase.
            if (command.isSource)
            {
                m_authority.Retire(command.character);
            }
            return HandoffResult::Duplicate;
        }
        if (!(aborted && TFHandoffState::Decode(row.migrationPayload, state)) && !ReadReservation(command, row, state))
        {
            return HandoffResult::Rejected;
        }
        if (!command.isSource)
        {
            return HandoffResult::Applied;
        }
        if (!m_database.AbortMigration(command.character, command.operation) ||
            !m_database.FindCharacter(command.character, row) || !m_authority.Install(row, state))
        {
            return HandoffResult::Unavailable;
        }
        return HandoffResult::Applied;
    }
} // namespace Terrafront
#endif
