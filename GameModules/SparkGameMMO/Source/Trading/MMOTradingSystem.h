/**
 * @file MMOTradingSystem.h
 * @brief Player-to-player trading and auction house
 * @author Spark Engine Team
 * @date 2026
 *
 * Supports direct trades with confirmation flow and an auction house
 * with listing, bidding, buyout, and timed expiry.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/MMOEnums.h"
#include "Inventory/MMOInventorySystem.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMO
{

    /// @brief State of a direct trade
    enum class TradeState : uint8_t
    {
        Idle,
        Proposed,
        Open,
        Locked,
        Completed,
        Cancelled
    };

    /// @brief One side of a player-to-player trade
    struct TradeOffer
    {
        uint32_t playerId = 0;
        std::vector<ItemStack> items;
        int currency = 0;
        bool confirmed = false;
    };

    /// @brief An active trade session
    struct TradeSession
    {
        uint32_t tradeId = 0;
        TradeOffer sideA;
        TradeOffer sideB;
        TradeState state = TradeState::Idle;
        float timeout = 0.0f;
    };

    /// @brief An auction house listing
    struct AuctionListing
    {
        uint32_t listingId = 0;
        uint32_t sellerId = 0;
        std::string sellerName;
        uint32_t itemDefId = 0;
        int itemCount = 1;
        int startPrice = 0;
        int buyoutPrice = 0;
        int currentBid = 0;
        uint32_t highBidderId = 0;
        std::string highBidderName;
        AuctionDuration duration = AuctionDuration::Medium;
        float timeRemaining = 0.0f;
        bool sold = false;
        bool expired = false;
    };

    /**
     * @brief Direct trading and auction house management
     */
    class MMOTradingSystem
    {
      public:
        MMOTradingSystem() = default;
        ~MMOTradingSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float dt);
        void Shutdown();
        void RenderDebugUI();

        // === Direct Trading ===
        uint32_t ProposeTrade(uint32_t initiatorId, uint32_t targetId);
        bool AcceptTrade(uint32_t tradeId, uint32_t playerId);
        bool CancelTrade(uint32_t tradeId, uint32_t playerId);
        bool AddTradeItem(uint32_t tradeId, uint32_t playerId, uint32_t itemDefId, int count);
        bool SetTradeCurrency(uint32_t tradeId, uint32_t playerId, int amount);
        bool ConfirmTrade(uint32_t tradeId, uint32_t playerId);
        bool ExecuteTrade(uint32_t tradeId, InventoryData& invA, InventoryData& invB, MMOInventorySystem& invSys);

        // === Auction House ===
        uint32_t CreateListing(uint32_t sellerId, const std::string& sellerName, uint32_t itemDefId, int count,
                               int startPrice, int buyoutPrice, AuctionDuration duration);
        bool PlaceBid(uint32_t listingId, uint32_t bidderId, const std::string& bidderName, int amount);
        bool Buyout(uint32_t listingId, uint32_t buyerId, InventoryData& buyerInv, MMOInventorySystem& invSys);
        bool CancelListing(uint32_t listingId, uint32_t sellerId);
        std::vector<const AuctionListing*> SearchListings(const std::string& term,
                                                          const MMOInventorySystem& invSys) const;

        size_t GetListingCount() const { return m_listings.size(); }
        std::string GetAuctionListString() const;

      private:
        float GetDurationSeconds(AuctionDuration d) const;
        TradeSession* GetTradeMut(uint32_t tradeId);

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, TradeSession> m_trades;
        std::vector<AuctionListing> m_listings;
        uint32_t m_nextTradeId = 1;
        uint32_t m_nextListingId = 1;

        static constexpr float TRADE_TIMEOUT = 120.0f;
        static constexpr int MAX_LISTINGS = 500;
    };

} // namespace MMO
