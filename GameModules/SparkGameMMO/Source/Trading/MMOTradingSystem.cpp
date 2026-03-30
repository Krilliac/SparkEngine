/**
 * @file MMOTradingSystem.cpp
 * @brief Direct trading and auction house implementation
 */

#include "MMOTradingSystem.h"
#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>
#include "Utils/LogMacros.h"

namespace MMO
{

    bool MMOTradingSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Trading system initialized");
        return true;
    }

    void MMOTradingSystem::Update(float dt)
    {
        // Expire trade sessions
        for (auto it = m_trades.begin(); it != m_trades.end();)
        {
            auto& trade = it->second;
            if (trade.state == TradeState::Open || trade.state == TradeState::Proposed)
            {
                trade.timeout += dt;
                if (trade.timeout >= TRADE_TIMEOUT)
                {
                    trade.state = TradeState::Cancelled;
                    it = m_trades.erase(it);
                    continue;
                }
            }
            ++it;
        }

        // Expire auction listings
        for (auto& listing : m_listings)
        {
            if (!listing.sold && !listing.expired)
            {
                listing.timeRemaining -= dt;
                if (listing.timeRemaining <= 0.0f)
                {
                    listing.expired = true;
                    // If there was a bid, the high bidder wins
                    if (listing.currentBid > 0 && listing.highBidderId != 0)
                        listing.sold = true;
                }
            }
        }

        // Clean up old expired/sold listings
        m_listings.erase(std::remove_if(m_listings.begin(), m_listings.end(),
                                        [](const AuctionListing& l) { return l.expired && !l.sold; }),
                         m_listings.end());
    }

    void MMOTradingSystem::Shutdown()
    {
        m_trades.clear();
        m_listings.clear();
    }

    // === Direct Trading ===

    uint32_t MMOTradingSystem::ProposeTrade(uint32_t initiatorId, uint32_t targetId)
    {
        TradeSession session;
        session.tradeId = m_nextTradeId++;
        session.sideA.playerId = initiatorId;
        session.sideB.playerId = targetId;
        session.state = TradeState::Proposed;
        uint32_t id = session.tradeId;
        m_trades[id] = std::move(session);
        return id;
    }

    bool MMOTradingSystem::AcceptTrade(uint32_t tradeId, uint32_t playerId)
    {
        auto* trade = GetTradeMut(tradeId);
        if (!trade || trade->state != TradeState::Proposed)
            return false;
        if (trade->sideB.playerId != playerId)
            return false;
        trade->state = TradeState::Open;
        return true;
    }

    bool MMOTradingSystem::CancelTrade(uint32_t tradeId, uint32_t playerId)
    {
        auto* trade = GetTradeMut(tradeId);
        if (!trade)
            return false;
        if (trade->sideA.playerId != playerId && trade->sideB.playerId != playerId)
            return false;
        trade->state = TradeState::Cancelled;
        m_trades.erase(tradeId);
        return true;
    }

    bool MMOTradingSystem::AddTradeItem(uint32_t tradeId, uint32_t playerId, uint32_t itemDefId, int count)
    {
        auto* trade = GetTradeMut(tradeId);
        if (!trade || trade->state != TradeState::Open)
            return false;

        TradeOffer* offer = nullptr;
        if (trade->sideA.playerId == playerId)
            offer = &trade->sideA;
        else if (trade->sideB.playerId == playerId)
            offer = &trade->sideB;
        if (!offer)
            return false;

        // Reset confirmation when items change
        offer->confirmed = false;
        trade->sideA.confirmed = false;
        trade->sideB.confirmed = false;

        offer->items.push_back({itemDefId, count});
        return true;
    }

    bool MMOTradingSystem::SetTradeCurrency(uint32_t tradeId, uint32_t playerId, int amount)
    {
        auto* trade = GetTradeMut(tradeId);
        if (!trade || trade->state != TradeState::Open || amount < 0)
            return false;

        if (trade->sideA.playerId == playerId)
        {
            trade->sideA.currency = amount;
            trade->sideA.confirmed = false;
            trade->sideB.confirmed = false;
        }
        else if (trade->sideB.playerId == playerId)
        {
            trade->sideB.currency = amount;
            trade->sideA.confirmed = false;
            trade->sideB.confirmed = false;
        }
        else
            return false;

        return true;
    }

    bool MMOTradingSystem::ConfirmTrade(uint32_t tradeId, uint32_t playerId)
    {
        auto* trade = GetTradeMut(tradeId);
        if (!trade || trade->state != TradeState::Open)
            return false;

        if (trade->sideA.playerId == playerId)
            trade->sideA.confirmed = true;
        else if (trade->sideB.playerId == playerId)
            trade->sideB.confirmed = true;
        else
            return false;

        if (trade->sideA.confirmed && trade->sideB.confirmed)
            trade->state = TradeState::Locked;

        return true;
    }

    bool MMOTradingSystem::ExecuteTrade(uint32_t tradeId, InventoryData& invA, InventoryData& invB,
                                        MMOInventorySystem& invSys)
    {
        auto* trade = GetTradeMut(tradeId);
        if (!trade || trade->state != TradeState::Locked)
            return false;

        // Transfer items A → B
        for (const auto& item : trade->sideA.items)
            invSys.AddItem(invB, item.itemDefId, item.count);

        // Transfer items B → A
        for (const auto& item : trade->sideB.items)
            invSys.AddItem(invA, item.itemDefId, item.count);

        // Transfer currency
        invA.currency -= trade->sideA.currency;
        invB.currency += trade->sideA.currency;
        invB.currency -= trade->sideB.currency;
        invA.currency += trade->sideB.currency;

        trade->state = TradeState::Completed;
        m_trades.erase(tradeId);
        return true;
    }

    TradeSession* MMOTradingSystem::GetTradeMut(uint32_t tradeId)
    {
        auto it = m_trades.find(tradeId);
        return it != m_trades.end() ? &it->second : nullptr;
    }

    // === Auction House ===

    float MMOTradingSystem::GetDurationSeconds(AuctionDuration d) const
    {
        switch (d)
        {
        case AuctionDuration::Short:
            return 43200.0f; // 12 hours
        case AuctionDuration::Medium:
            return 86400.0f; // 24 hours
        case AuctionDuration::Long:
            return 172800.0f; // 48 hours
        default:
            return 86400.0f;
        }
    }

    uint32_t MMOTradingSystem::CreateListing(uint32_t sellerId, const std::string& sellerName, uint32_t itemDefId,
                                             int count, int startPrice, int buyoutPrice, AuctionDuration duration)
    {
        if (static_cast<int>(m_listings.size()) >= MAX_LISTINGS)
            return 0;

        AuctionListing listing;
        listing.listingId = m_nextListingId++;
        listing.sellerId = sellerId;
        listing.sellerName = sellerName;
        listing.itemDefId = itemDefId;
        listing.itemCount = count;
        listing.startPrice = startPrice;
        listing.buyoutPrice = buyoutPrice;
        listing.currentBid = 0;
        listing.duration = duration;
        listing.timeRemaining = GetDurationSeconds(duration);

        uint32_t id = listing.listingId;
        m_listings.push_back(std::move(listing));
        return id;
    }

    bool MMOTradingSystem::PlaceBid(uint32_t listingId, uint32_t bidderId, const std::string& bidderName, int amount)
    {
        for (auto& listing : m_listings)
        {
            if (listing.listingId == listingId && !listing.sold && !listing.expired)
            {
                if (amount <= listing.currentBid || amount < listing.startPrice)
                    return false;
                if (listing.sellerId == bidderId)
                    return false;

                listing.currentBid = amount;
                listing.highBidderId = bidderId;
                listing.highBidderName = bidderName;
                return true;
            }
        }
        return false;
    }

    bool MMOTradingSystem::Buyout(uint32_t listingId, uint32_t buyerId, InventoryData& buyerInv,
                                  MMOInventorySystem& invSys)
    {
        for (auto& listing : m_listings)
        {
            if (listing.listingId == listingId && !listing.sold && !listing.expired)
            {
                if (listing.buyoutPrice <= 0 || listing.sellerId == buyerId)
                    return false;
                if (buyerInv.currency < listing.buyoutPrice)
                    return false;

                buyerInv.currency -= listing.buyoutPrice;
                invSys.AddItem(buyerInv, listing.itemDefId, listing.itemCount);
                listing.sold = true;
                listing.highBidderId = buyerId;
                listing.currentBid = listing.buyoutPrice;
                return true;
            }
        }
        return false;
    }

    bool MMOTradingSystem::CancelListing(uint32_t listingId, uint32_t sellerId)
    {
        for (auto it = m_listings.begin(); it != m_listings.end(); ++it)
        {
            if (it->listingId == listingId && it->sellerId == sellerId && it->currentBid == 0 && !it->sold)
            {
                m_listings.erase(it);
                return true;
            }
        }
        return false;
    }

    std::vector<const AuctionListing*> MMOTradingSystem::SearchListings(const std::string& term,
                                                                        const MMOInventorySystem& invSys) const
    {
        std::vector<const AuctionListing*> results;
        for (const auto& listing : m_listings)
        {
            if (listing.sold || listing.expired)
                continue;
            const auto* def = invSys.GetItem(listing.itemDefId);
            if (def && def->name.find(term) != std::string::npos)
                results.push_back(&listing);
        }
        return results;
    }

    std::string MMOTradingSystem::GetAuctionListString() const
    {
        std::ostringstream ss;
        int active = 0;
        for (const auto& l : m_listings)
        {
            if (!l.sold && !l.expired)
                ++active;
        }
        ss << "Auction House (" << active << " active listings):\n";
        for (const auto& l : m_listings)
        {
            if (l.sold || l.expired)
                continue;
            ss << "  [" << l.listingId << "] Item#" << l.itemDefId << " x" << l.itemCount
               << " - Start: " << l.startPrice << " Buyout: " << l.buyoutPrice << " Bid: " << l.currentBid << "\n";
        }
        return ss.str();
    }

    void MMOTradingSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Trading System"))
        {
            ImGui::Text("Active Trades: %zu", m_trades.size());
            ImGui::Text("Auction Listings: %zu", m_listings.size());

            if (ImGui::TreeNode("Auction House"))
            {
                for (const auto& l : m_listings)
                {
                    if (l.sold || l.expired)
                        continue;
                    ImGui::Text("[%u] Item#%u x%d | Start:%d Buyout:%d Bid:%d by %s", l.listingId, l.itemDefId,
                                l.itemCount, l.startPrice, l.buyoutPrice, l.currentBid,
                                l.highBidderName.empty() ? "none" : l.highBidderName.c_str());
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
