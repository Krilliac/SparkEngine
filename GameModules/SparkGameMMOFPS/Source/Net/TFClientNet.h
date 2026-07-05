/**
 * @file TFClientNet.h
 * @brief Client connection, ClientPrediction wiring, interpolation buffers.
 *
 * OWNERSHIP: this header + TFClientNet.cpp belong to ONE implementation agent.
 * The lifecycle + the FROZEN cross-system API below are the module contract.
 *
 * W1 status: minimal contract-satisfying implementation (connection state +
 * raw sends via NetworkManager). The full W1 body — handshake/TF_WorldWelcome,
 * client-side TFMsg handlers, input pump, ClientPrediction + reconciliation,
 * remote-entity interpolation, first-person camera — is the client-player
 * agent's deliverable and replaces TFClientNet.cpp wholesale.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"

#include <string>

namespace Terrafront {

class TFClientNet {
  public:
    TFClientNet();
    ~TFClientNet();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    // --- FROZEN cross-system API (W1) ---
    bool     IsConnected() const;
    PlayerId LocalPlayerId() const;
    void     SendInput(const TF_ClientInput& input);
    void     SendMsg(TFMsg id, const void* payload, size_t size);

    /// Connect to a remote host (called via TFWorldSetup::Connect).
    bool Connect(const std::string& ip, uint16_t port);
    void Disconnect();

  private:
    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
    bool           m_connected{false};
    PlayerId       m_localPlayer{kInvalidPlayer};
    uint32_t       m_inputSeq{0};
};

} // namespace Terrafront
