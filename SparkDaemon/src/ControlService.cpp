/**
 * @file ControlService.cpp
 * @brief Implementation of the built-in Control service.
 */

#include "ControlService.h"

#include <string_view>

namespace Spark::Daemon
{

    std::optional<ServiceResponse> ControlService::HandleMessage(uint16_t messageType,
                                                                 const std::vector<uint8_t>& payload)
    {
        (void)payload;
        switch (static_cast<ControlMessage>(messageType))
        {
        case ControlMessage::PingRequest:
        {
            constexpr std::string_view pong = "pong";
            ServiceResponse r;
            r.messageType = static_cast<uint16_t>(ControlMessage::PingResponse);
            r.payload.assign(pong.begin(), pong.end());
            return r;
        }
        case ControlMessage::VersionRequest:
        {
            std::string_view v = kProtocolVersion;
            ServiceResponse r;
            r.messageType = static_cast<uint16_t>(ControlMessage::VersionResponse);
            r.payload.assign(v.begin(), v.end());
            return r;
        }
        case ControlMessage::ShutdownRequest:
        {
            m_shouldStop.store(true, std::memory_order_release);
            ServiceResponse r;
            r.messageType = static_cast<uint16_t>(ControlMessage::ShutdownAck);
            return r;
        }
        default:
        {
            std::string_view msg = "unsupported control message";
            ServiceResponse r;
            r.messageType = static_cast<uint16_t>(ControlMessage::ErrorResponse);
            r.payload.assign(msg.begin(), msg.end());
            return r;
        }
        }
    }

} // namespace Spark::Daemon
