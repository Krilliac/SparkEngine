/**
 * @file FPSLANIntruderTransport.cpp
 * @brief MOD-315: the FPSLAN intruder peer's bare UDP socket, in its own translation unit.
 *
 * The intruder role of Tests/Fixtures/FPSLANLoopbackPeer.cpp sends hostile datagrams through
 * the engine's UDPTransport rather than socket code of its own. UDPTransport.h and
 * NetworkManager.h both declare the POSIX socket shims (SOCKET, INVALID_SOCKET, SOCKET_ERROR),
 * so no translation unit can include both; the peer needs NetworkManager.h, so the transport is
 * constructed here and handed over as an ITransport.
 *
 * Contract: called once from the intruder's only thread; the caller owns the transport.
 */

#include "Engine/Networking/UDPTransport.h"

#include <memory>

std::unique_ptr<Spark::Net::ITransport> MakeFPSLANIntruderTransport()
{
    return std::make_unique<Spark::Net::UDPTransport>();
}
