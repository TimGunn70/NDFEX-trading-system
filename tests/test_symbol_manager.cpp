#include "test_main.h"

#include "symbol_manager.h"

#include <cstdint>
#include <vector>
#include <cstdio>

// AUDIT.md C1 — PnL accounting in SymbolManager::on_fill.
//
// Table-driven: each row is one fill applied to a single symbol, in order,
// starting from flat. After every fill the test checks the position and the
// running realized PnL. Rows are cumulative — row N's expected values assume
// rows 1..N-1 have already been applied.
//
// Accounting rules this table encodes (average-cost method):
//   - Opening or adding to a position updates the average entry price
//     (weighted average; from flat it is simply the fill price).
//   - Reducing or closing realizes PnL against the average and leaves the
//     average unchanged.
//   - A flip is a close of the whole old position followed by an open of the
//     remainder at the fill price.

namespace {

constexpr uint32_t SYMBOL = SYM_GOLD;

struct FillCase {
    const char* name;

    // The fill
    SIDE     side;
    uint32_t qty;
    int32_t  price;

    // Expected state after the fill
    int32_t position;
    double  avg_entry;       // not checked yet: SymbolManager has no getter for it
    double  total_realized;
};

const std::vector<FillCase> CASES = {
    //  name          side         qty  price   pos   avg      total
    { "open long",    SIDE::BUY,    10,  100,   10,  100.0,     0.0 },
    { "add",          SIDE::BUY,    10,  110,   20,  105.0,     0.0 },
    { "reduce",       SIDE::SELL,    5,  120,   15,  105.0,    75.0 },
    { "close",        SIDE::SELL,   15,   90,    0,  105.0,  -150.0 },
    { "open short",   SIDE::SELL,   10,  100,  -10,  100.0,  -150.0 },
    { "cover",        SIDE::BUY,     4,   95,   -6,  100.0,  -130.0 },
    { "flip",         SIDE::BUY,    10,  105,    4,  105.0,  -160.0 },
    { "close after flip", SIDE::SELL,   4,  110,    0,  105.0,  -140.0 },
    { "open short again", SIDE::SELL,  10,  100,  -10,  100.0,  -140.0 },
    { "add to short", SIDE::SELL,  10,   90,  -20,   95.0,  -140.0 },
    { "cover all",    SIDE::BUY,   20,   85,    0,   95.0,    60.0 },
};

}  // namespace

TEST(c1_fill_sequence_long_through_flip) {
    SymbolManager sm;

    for (const FillCase& c : CASES) {
        sm.on_fill(SYMBOL, c.side, c.qty, c.price);

        const int before = ::testing::failure_count(); 
        EXPECT_EQ(sm.get_position(SYMBOL), c.position);
        EXPECT_NEAR(sm.get_total_pnl(), c.total_realized, 1e-9);
        if (::testing::failure_count() != before) {  
            std::printf("    ^ row: %s\n", c.name);
        }
    }
}
