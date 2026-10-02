/**
 * @file GatewayCoordinator.cpp
 * @brief Fenced gateway session state machine implementation.
 */

#include "GatewayCoordinator.h"

#include "GatewayLocalAdapters.h"

#include <algorithm>
#include <cmath>

namespace Spark::Gateway
{
    namespace
    {
        bool IsSafeText(std::string_view value)
        {
            return std::ranges::none_of(value, [](char character)
                                        { return character == '\0' || character == '\r' || character == '\n'; });
        }
    } // namespace

    GatewayCoordinator::GatewayCoordinator(Net::WorldServer& worldServer, IGatewayAuthenticator& authenticator,
                                           IAreaControlPlane& controlPlane)
        : m_worldServer(&worldServer), m_authenticator(std::make_unique<GuardedGatewayAuthenticator>(authenticator)),
          m_controlPlane(&controlPlane), m_ownedPlacement(std::make_unique<LocalDeterministicPlacement>()),
          m_placement(m_ownedPlacement.get())
    {
    }

    GatewayCoordinator::GatewayCoordinator(Net::WorldServer& worldServer, IGatewayAuthenticator& authenticator,
                                           IAreaControlPlane& controlPlane, IAreaPlacementPolicy& placement)
        : m_worldServer(&worldServer), m_authenticator(std::make_unique<GuardedGatewayAuthenticator>(authenticator)),
          m_controlPlane(&controlPlane), m_placement(&placement)
    {
    }

    GatewayCoordinator::~GatewayCoordinator() = default;

    GatewayAuthenticatorHealth GatewayCoordinator::GetAuthenticationHealth() const
    {
        return m_authenticator->GetHealth();
    }

    bool GatewayCoordinator::RegisterAreas(const std::vector<AreaEndpoint>& endpoints)
    {
        std::lock_guard lock(m_mutex);
        if (!m_endpoints.empty() || !m_sessions.empty())
            return false;

        for (const AreaEndpoint& endpoint : endpoints)
        {
            if (endpoint.area.areaName.empty() || endpoint.host != "127.0.0.1" || endpoint.area.port == 0 ||
                endpoint.area.maxClients <= 0)
                return false;
        }
        for (const AreaEndpoint& endpoint : endpoints)
        {
            const Net::AreaID id = m_worldServer->RegisterAreaServer(endpoint.area);
            if (id == Net::INVALID_AREA)
                return false;
            m_endpoints.emplace_back(id, endpoint);
            m_controlPlane->RegisterEndpoint(id, endpoint);
        }
        return !m_endpoints.empty();
    }

    RouteResult GatewayCoordinator::Admit(const AdmissionRequest& request)
    {
        RouteResult result;
        if (!IsRoutable())
        {
            result.failure = RouteFailure::NotReady;
            result.reason = "Gateway admission or area control plane is not ready";
            return result;
        }
        if (request.clientId == Net::INVALID_CLIENT || request.sessionId.empty() || request.sessionId.size() > 128 ||
            !IsSafeText(request.sessionId) || request.playerName.empty() || request.playerName.size() > 64 ||
            !IsSafeText(request.playerName) || request.credential.empty() || request.credential.size() > 4096 ||
            !std::isfinite(request.spawnPosition.x) || !std::isfinite(request.spawnPosition.y) ||
            !std::isfinite(request.spawnPosition.z))
        {
            result.failure = RouteFailure::InvalidRequest;
            result.reason = "Admission request failed structural validation";
            return result;
        }

        // Never retain or log the opaque credential.
        AuthenticationResult authentication = m_authenticator->Authenticate(request);
        if (!authentication.accepted || authentication.principalId.empty())
        {
            result.failure = RouteFailure::AuthenticationFailed;
            result.reason = authentication.reason.empty() ? "Authentication failed" : std::move(authentication.reason);
            return result;
        }

        std::lock_guard lock(m_mutex);
        if (!m_accepting)
        {
            result.failure = RouteFailure::NotReady;
            result.reason = "Gateway is draining";
            return result;
        }
        if (m_sessions.contains(request.sessionId))
        {
            result.failure = RouteFailure::DuplicateSession;
            result.reason = "Session identifier is already active";
            return result;
        }
        if (m_sessionByClient.contains(request.clientId))
        {
            result.failure = RouteFailure::DuplicateSession;
            result.reason = "Client identifier is already bound to an active session";
            return result;
        }
        // The gateway's own session table is capped independently of the WorldServer mirror, so
        // the world limit holds even if the mirror and the gateway ever disagree.
        const size_t maxTotalClients = static_cast<size_t>(std::max(0, m_worldServer->GetConfig().maxTotalClients));
        if (m_sessions.size() >= maxTotalClients || m_worldServer->GetTotalPlayerCount() >= maxTotalClients)
        {
            result.failure = RouteFailure::CapacityReached;
            result.reason = "World capacity reached";
            return result;
        }

        // Placement (boundary B9): the policy chooses; the coordinator only accepts an area that is
        // registered, online and below capacity, whatever the policy returned.
        const std::vector<AreaSnapshot> areas = SnapshotAreas();
        const PlacementRequest placement{authentication.principalId, request.sessionId, request.spawnPosition};
        Net::AreaID area = Net::INVALID_AREA;
        try
        {
            area = m_placement->Place(placement, areas);
        }
        catch (...)
        {
            result.failure = RouteFailure::NoAreaAvailable;
            result.reason = "Area placement policy fault";
            return result;
        }
        const auto placed = std::ranges::find(areas, area, &AreaSnapshot::areaId);
        const AreaEndpoint* endpoint = FindEndpoint(area);
        if (placed == areas.end() || !placed->online || placed->sessions >= placed->capacity || endpoint == nullptr ||
            !m_worldServer->HandlePlayerConnectToArea(request.clientId, request.playerName, request.spawnPosition,
                                                      area))
        {
            result.failure = RouteFailure::NoAreaAvailable;
            result.reason = "No healthy area with free capacity is available";
            return result;
        }

        SessionRecord record;
        record.snapshot.sessionId = request.sessionId;
        record.snapshot.principalId = std::move(authentication.principalId);
        record.snapshot.clientId = request.clientId;
        record.snapshot.authoritativeArea = area;
        record.snapshot.state = SessionState::Active;
        result.accepted = true;
        result.session = record.snapshot;
        result.host = endpoint->host;
        result.port = endpoint->area.port;
        m_sessionByClient.emplace(request.clientId, request.sessionId);
        m_sessions.emplace(request.sessionId, std::move(record));
        return result;
    }

    std::optional<uint64_t> GatewayCoordinator::BeginHandoff(std::string_view sessionId, Net::AreaID targetArea)
    {
        std::lock_guard lock(m_mutex);
        auto found = m_sessions.find(std::string(sessionId));
        if (found == m_sessions.end() || FindEndpoint(targetArea) == nullptr)
            return std::nullopt;

        SessionSnapshot& session = found->second.snapshot;
        if (session.state != SessionState::Active)
        {
            if (session.targetArea == targetArea)
                return session.epoch;
            return std::nullopt;
        }
        if (session.authoritativeArea == targetArea)
            return std::nullopt;

        ++session.epoch;
        session.targetArea = targetArea;
        session.state = SessionState::Preparing;
        return session.epoch;
    }

    AdvanceResult GatewayCoordinator::AdvanceHandoff(std::string_view sessionId, uint64_t epoch)
    {
        HandoffCommand command;
        SessionState phase = SessionState::Active;
        {
            std::lock_guard lock(m_mutex);
            auto found = m_sessions.find(std::string(sessionId));
            if (found == m_sessions.end())
                return AdvanceResult::InvalidSession;
            const SessionSnapshot& session = found->second.snapshot;
            if (session.epoch != epoch)
                return AdvanceResult::StaleEpoch;
            if (session.state == SessionState::Active)
                return AdvanceResult::InvalidState;
            phase = session.state;
            command = {session.sessionId, session.epoch, session.authoritativeArea, session.targetArea};
        }

        HandoffOperationResult operation = HandoffOperationResult::Rejected;
        switch (phase)
        {
        case SessionState::Preparing:
            operation = m_controlPlane->Prepare(command);
            break;
        case SessionState::Transferring:
            operation = m_controlPlane->Transfer(command);
            break;
        case SessionState::Committing:
            operation = m_controlPlane->Commit(command);
            break;
        case SessionState::AwaitingAcknowledgement:
            operation = m_controlPlane->Acknowledge(command);
            break;
        case SessionState::Aborting:
            operation = m_controlPlane->Abort(command);
            break;
        case SessionState::Active:
            return AdvanceResult::InvalidState;
        }

        std::lock_guard lock(m_mutex);
        auto found = m_sessions.find(std::string(sessionId));
        if (found == m_sessions.end())
            return AdvanceResult::InvalidSession;
        SessionRecord& record = found->second;
        if (record.snapshot.epoch != epoch)
            return AdvanceResult::StaleEpoch;
        if (record.snapshot.state != phase)
            return AdvanceResult::InvalidState;

        if (operation == HandoffOperationResult::Unavailable)
            return AdvanceResult::WaitingForRetry;
        if (operation == HandoffOperationResult::Rejected)
        {
            if (phase != SessionState::Aborting)
            {
                record.snapshot.state = SessionState::Aborting;
                return AdvanceResult::Aborting;
            }
            return AdvanceResult::WaitingForRetry;
        }

        switch (phase)
        {
        case SessionState::Preparing:
            record.snapshot.state = SessionState::Transferring;
            return AdvanceResult::Advanced;
        case SessionState::Transferring:
            record.snapshot.state = SessionState::Committing;
            return AdvanceResult::Advanced;
        case SessionState::Committing:
            record.snapshot.state = SessionState::AwaitingAcknowledgement;
            return AdvanceResult::Advanced;
        case SessionState::AwaitingAcknowledgement:
            // External source authority has now acknowledged the committed
            // target. Refresh WorldServer's routing mirror best-effort; this
            // mirror must never reverse an already-resolved server handoff.
            (void)m_worldServer->TransferPlayer(record.snapshot.clientId, record.snapshot.targetArea);
            record.snapshot.authoritativeArea = record.snapshot.targetArea;
            record.snapshot.targetArea = Net::INVALID_AREA;
            record.snapshot.state = SessionState::Active;
            return AdvanceResult::Completed;
        case SessionState::Aborting:
            // Source authority was never discarded; clear only the pending target.
            record.snapshot.targetArea = Net::INVALID_AREA;
            record.snapshot.state = SessionState::Active;
            return AdvanceResult::Completed;
        case SessionState::Active:
            return AdvanceResult::InvalidState;
        }
        return AdvanceResult::InvalidState;
    }

    bool GatewayCoordinator::Disconnect(std::string_view sessionId)
    {
        std::lock_guard lock(m_mutex);
        auto found = m_sessions.find(std::string(sessionId));
        if (found == m_sessions.end() || found->second.snapshot.state != SessionState::Active)
            return false;
        m_worldServer->HandlePlayerDisconnect(found->second.snapshot.clientId);
        m_sessionByClient.erase(found->second.snapshot.clientId);
        m_sessions.erase(found);
        return true;
    }

    void GatewayCoordinator::BeginDrain()
    {
        std::lock_guard lock(m_mutex);
        m_accepting = false;
    }

    bool GatewayCoordinator::CanShutdown() const
    {
        std::lock_guard lock(m_mutex);
        for (const auto& [sessionId, record] : m_sessions)
        {
            (void)sessionId;
            if (record.snapshot.state != SessionState::Active)
                return false;
        }
        return true;
    }

    std::optional<SessionSnapshot> GatewayCoordinator::GetSession(std::string_view sessionId) const
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_sessions.find(std::string(sessionId));
        if (found == m_sessions.end())
            return std::nullopt;
        return found->second.snapshot;
    }

    size_t GatewayCoordinator::GetSessionCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_sessions.size();
    }

    bool GatewayCoordinator::IsReady() const
    {
        return IsRoutable() && !m_authenticator->IsFailingFast();
    }

    bool GatewayCoordinator::IsRoutable() const
    {
        std::vector<Net::AreaID> areas;
        Net::WorldServer* world = nullptr;
        IAreaControlPlane* control = nullptr;
        bool accepting = false;
        {
            std::lock_guard lock(m_mutex);
            world = m_worldServer;
            control = m_controlPlane;
            accepting = m_accepting;
            areas.reserve(m_endpoints.size());
            for (const auto& [id, endpoint] : m_endpoints)
            {
                (void)endpoint;
                areas.push_back(id);
            }
        }
        if (!world || !world->IsRunning() || !m_authenticator->IsReady() || !control || areas.empty() || !accepting)
            return false;

        bool allReady = true;
        for (const Net::AreaID id : areas)
        {
            const bool ready = control->IsEndpointReady(id);
            (void)world->SetAreaOnline(id, ready);
            allReady &= ready;
        }
        return allReady;
    }

    std::vector<AreaSnapshot> GatewayCoordinator::SnapshotAreas() const
    {
        std::vector<AreaSnapshot> areas;
        areas.reserve(m_endpoints.size());
        for (const auto& [id, endpoint] : m_endpoints)
        {
            AreaSnapshot area;
            area.areaId = id;
            const std::optional<Net::AreaRegistration> registration = m_worldServer->GetAreaInfoSnapshot(id);
            area.online = registration.has_value() && registration->isOnline;
            area.capacity = static_cast<uint32_t>(endpoint.area.maxClients);
            area.sessions = static_cast<uint32_t>(std::ranges::count_if(
                m_sessions, [id](const auto& entry) { return entry.second.snapshot.authoritativeArea == id; }));
            areas.push_back(area);
        }
        return areas;
    }

    const AreaEndpoint* GatewayCoordinator::FindEndpoint(Net::AreaID areaId) const
    {
        for (const auto& [registeredId, endpoint] : m_endpoints)
        {
            if (registeredId == areaId)
                return &endpoint;
        }
        return nullptr;
    }
} // namespace Spark::Gateway
