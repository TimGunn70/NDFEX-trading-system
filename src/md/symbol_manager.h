#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <cstdint>

#include "orderbook.h"
#include "messages.h"


static constexpr uint32_t SYM_GOLD = 1;
static constexpr uint32_t SYM_BLUE = 2;
static constexpr uint32_t SYM_KNAN = 3;
static constexpr uint32_t SYM_STED = 4;
static constexpr uint32_t SYM_FISH = 5;
static constexpr uint32_t SYM_DILN = 6;
static constexpr uint32_t SYM_SORN = 7;
static constexpr uint32_t SYM_RYAN = 8;
static constexpr uint32_t SYM_LYON = 9;
static constexpr uint32_t SYM_WLSH = 10;
static constexpr uint32_t SYM_LEWI = 11;
static constexpr uint32_t SYM_BDIN = 12;
static constexpr uint32_t SYM_UNDY = 13;

static constexpr std::array<uint32_t, 10> DORM_IDS = {
    SYM_KNAN, SYM_STED, SYM_FISH, SYM_DILN, SYM_SORN,
    SYM_RYAN, SYM_LYON, SYM_WLSH, SYM_LEWI, SYM_BDIN
};

struct ArbSnapshot {
    struct DormData {
        int32_t  best_bid_price;
        int32_t  best_ask_price;
        uint32_t best_bid_qty;
        uint32_t best_ask_qty;
        int32_t  position;
    };
    std::array<DormData, 10> dorms; 

    int32_t  undy_best_bid_price;
    int32_t  undy_best_ask_price;
    uint32_t undy_best_bid_qty;
    uint32_t undy_best_ask_qty;
    int32_t  undy_position;

    
    int32_t nav_ask;               // sum of all 10 dorm best asks
    int32_t nav_bid;               // sum of all 10 dorm best bids
    bool    any_dorm_ask_missing;  // true if any dorm has no ask
    bool    any_dorm_bid_missing;  // true if any dorm has no bid
};


class SymbolManager {
public:
    static constexpr int32_t POSITION_LIMIT  = 10;
    static constexpr double  PNL_WARN_LEVEL  = -4500.0;

    SymbolManager();


    void on_new_order   (uint32_t symbol_id, const new_order*    msg);
    void on_delete_order(uint32_t symbol_id, const delete_order* msg);
    void on_modify_order(uint32_t symbol_id, const modify_order* msg);
    void on_trade       (uint32_t symbol_id, const trade*        msg);

    void reset_book(uint32_t symbol_id);


    void on_fill(uint32_t symbol_id, SIDE side, uint32_t qty, int32_t price);


    ArbSnapshot snapshot() const;

    int32_t  best_bid_price(uint32_t symbol_id) const;
    int32_t  best_ask_price(uint32_t symbol_id) const;
    uint32_t best_bid_qty  (uint32_t symbol_id) const;
    uint32_t best_ask_qty  (uint32_t symbol_id) const;
    int32_t  get_position  (uint32_t symbol_id) const;

    bool would_breach_limit(uint32_t symbol_id, SIDE side, int32_t qty) const;

    double get_total_pnl()  const;
    bool   pnl_near_limit() const;

private:
    struct SymbolSlot {
        OrderBook book;

        std::atomic<int32_t>  best_bid_price{0};
        std::atomic<uint32_t> best_bid_qty{0};
        std::atomic<int32_t>  best_ask_price{0};
        std::atomic<uint32_t> best_ask_qty{0};

        std::atomic<int32_t>  position{0};

        explicit SymbolSlot(uint32_t id) : book(id) {}

        SymbolSlot(const SymbolSlot&)            = delete;
        SymbolSlot& operator=(const SymbolSlot&) = delete;

        void flush_top_of_book() {
            best_bid_price.store(book.get_best_bid_price(), std::memory_order_release);
            best_bid_qty  .store(book.get_best_bid_qty(),   std::memory_order_release);
            best_ask_price.store(book.get_best_ask_price(), std::memory_order_release);
            best_ask_qty  .store(book.get_best_ask_qty(),   std::memory_order_release);
        }
    };

    std::array<std::unique_ptr<SymbolSlot>, 14> slots_; 

    std::atomic<double> total_pnl_{0.0};

    std::array<double, 14> avg_entry_price_{};

    SymbolSlot&       slot(uint32_t symbol_id);
    const SymbolSlot& slot(uint32_t symbol_id) const;
};