/**
 * @file TFClientNet.cpp
 * @brief Client connection + sends. (W1 minimal contract implementation —
 *        the client-player agent replaces this with the full handshake /
 *        prediction / interpolation / camera body.)
 */
#include "Net/TFClientNet.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstring>

namespace Terrafront {

TFClientNet::TFClientNet() = default;
TFClientNet::~TFClientNet() { if (m_initialized) Shutdown(); }

bool TFClientNet::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    return true;
}

void TFClientNet::Update(float) {}
void TFClientNet::FixedUpdate(float) {}

void TFClientNet::Shutdown()
{
    Disconnect();
    m_initialized = false;
}

void TFClientNet::RenderDebugUI() {}

bool TFClientNet::IsConnected() const
{
    return m_connected;
}

PlayerId TFClientNet::LocalPlayerId() const
{
    return m_localPlayer;
}

void TFClientNet::SendInput(const TF_ClientInput& input)
{
    SendMsg(TFMsg::ClientInput, &input, sizeof(input));
}

void TFClientNet::SendMsg(TFMsg id, const void* payload, size_t size)
{
#ifdef ENABLE_NETWORKING
    if (!m_connected)
        return;
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    Spark::Net::NetworkMessage msg;
    msg.type = static_cast<Spark::Net::MessageType>(static_cast<uint16_t>(id));
    msg.channel = Spark::Net::ChannelType::Reliable;
    msg.payload.resize(size);
    if (size > 0)
        std::memcpy(msg.payload.data(), payload, size);
    nm.SendMessage(msg);
#else
    (void)id; (void)payload; (void)size;
#endif
}

bool TFClientNet::Connect(const std::string& ip, uint16_t port)
{
#ifdef ENABLE_NETWORKING
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    if (!nm.IsInitialized())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Game,
                       "[TF] Connect: NetworkManager not initialized");
        return false;
    }
    // TF-W1-FULL: handshake (TF_WorldWelcome -> m_localPlayer), handler
    // registration, prediction wiring. Minimal body only marks intent.
    m_connected = true;
    if (m_ctx) m_ctx->role = NetRole::Client;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Connecting to %s:%u", ip.c_str(),
                   static_cast<unsigned>(port));
    return true;
#else
    (void)ip; (void)port;
    return false;
#endif
}

void TFClientNet::Disconnect()
{
    m_connected = false;
    m_localPlayer = kInvalidPlayer;
    if (m_ctx && m_ctx->role == NetRole::Client)
        m_ctx->role = NetRole::Standalone;
}

} // namespace Terrafront
