#include "test_main.h"

#include "snapshot_parser.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

// AUDIT.md P2 — the snapshot parser must not trust anything on the wire.
//
// Contract under test (see snapshot_parser.h):
//   OK            -> `out` holds every snapshot in the packet, in packet order.
//   anything else -> `out` is empty. No partial results, ever.
//
// Which error a malformed packet produces is a design decision. The choices
// these tests encode:
//   - fewer bytes left than the next struct needs, or than its header.length
//     claims                                        -> NOT_ENOUGH_BYTES
//   - header.length smaller than the struct it labels (including 0)
//                                                   -> INVALID_HEADER_LENGTH
//   - anything other than SNAPSHOT_INFO / NEW_ORDER where one is required
//                                                   -> UNKNOWN_MESSAGE_TYPE
//   - header.length LARGER than the struct, bytes present -> accepted; the
//     extra bytes are skipped (room for the protocol to grow).

// Lets EXPECT_EQ print the enum by name. Global namespace on purpose: that is
// SnapshotParseResult's namespace, so argument-dependent lookup finds this
// from inside testing::to_str. (In an anonymous namespace, ADL would miss it.)
static std::ostream& operator<<(std::ostream& os, SnapshotParseResult r) {
    return os << to_string(r);
}

namespace {

constexpr uint32_t SYM_A = 1;
constexpr uint32_t SYM_B = 2;

constexpr uint16_t SNAP_LEN  = sizeof(snapshot_info);
constexpr uint16_t ORDER_LEN = sizeof(new_order);

// Builds a packet byte by byte, the way it would arrive from recv().
// Every field a test does not mention is zero.
class Packet {
public:
    Packet& snapshot(uint32_t symbol, uint32_t bids, uint32_t asks, uint32_t seq,
                     uint16_t length = SNAP_LEN,
                     MSG_TYPE type = MSG_TYPE::SNAPSHOT_INFO) {
        snapshot_info s{};
        s.header          = header(type, length);
        s.symbol          = symbol;
        s.bid_count       = bids;
        s.ask_count       = asks;
        s.last_md_seq_num = seq;
        append(&s, sizeof s);
        return *this;
    }

    Packet& order(uint64_t id, uint32_t symbol, SIDE side, uint32_t qty, int32_t price,
                  uint16_t length = ORDER_LEN,
                  MSG_TYPE type = MSG_TYPE::NEW_ORDER) {
        new_order o{};
        o.header   = header(type, length);
        o.order_id = id;
        o.symbol   = symbol;
        o.side     = side;
        o.quantity = qty;
        o.price    = price;
        append(&o, sizeof o);
        return *this;
    }

    Packet& padding(size_t n) {
        bytes_.insert(bytes_.end(), n, '\xAB');
        return *this;
    }

    Packet& truncate(size_t n) {
        bytes_.resize(n);
        return *this;
    }

    const std::vector<char>& bytes() const { return bytes_; }

private:
    static md_header header(MSG_TYPE type, uint16_t length) {
        md_header h{};
        h.magic_number = SNAPSHOT_MAGIC_NUMBER;
        h.length       = length;
        h.msg_type     = type;
        return h;
    }

    void append(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        bytes_.insert(bytes_.end(), c, c + n);
    }

    std::vector<char> bytes_;
};

// Runs the parser the way every test should:
//
//  1. `out` starts with a stale entry, so every test also checks the parser
//     clears it instead of appending to whatever the caller left there.
//  2. The bytes are copied into a heap block of EXACTLY the packet's size.
//     A std::vector's capacity can exceed its size, so a read one byte past
//     the end might land in memory the vector owns and ASan would stay quiet.
//     With an exact-size block, any read past `bytes` is a heap overflow
//     ASan reports.
SnapshotParseResult parse(const Packet& p, std::vector<Snapshot>& out) {
    out.assign(1, Snapshot{});
    out[0].symbol = 999;

    const std::vector<char>& b = p.bytes();
    std::unique_ptr<char[]> exact(new char[b.size()]);
    if (!b.empty()) std::memcpy(exact.get(), b.data(), b.size());
    return parse_snapshot(exact.get(), b.size(), out);
}

// new_order is __attribute__((packed)), so its fields may be misaligned and a
// const reference cannot bind to them -- which is exactly what EXPECT_EQ does.
// static_cast to the field's own type makes a by-value copy that it can bind.
#define ORDER_FIELD(order, field) static_cast<decltype(new_order::field)>((order).field)

Packet valid_two_symbol_packet() {
    Packet p;
    p.snapshot(SYM_A, 1, 1, 100)
         .order(11, SYM_A, SIDE::BUY,  5, 99)
         .order(12, SYM_A, SIDE::SELL, 7, 101)
     .snapshot(SYM_B, 0, 1, 200)
         .order(21, SYM_B, SIDE::SELL, 3, 55);
    return p;
}

}  // namespace

// ── Valid packets ──────────────────────────────────────────────────────────

TEST(valid_single_snapshot) {
    Packet p;
    p.snapshot(SYM_A, 1, 1, 100)
         .order(11, SYM_A, SIDE::BUY,  5, 99)
         .order(12, SYM_A, SIDE::SELL, 7, 101);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::OK);
    EXPECT_EQ(out.size(), 1u);
    if (out.size() != 1) return;

    EXPECT_EQ(out[0].symbol, SYM_A);
    EXPECT_EQ(out[0].last_md_seq_num, 100u);
    EXPECT_EQ(out[0].orders.size(), 2u);
    if (out[0].orders.size() != 2) return;

    EXPECT_EQ(ORDER_FIELD(out[0].orders[0], order_id), 11u);
    EXPECT_TRUE(ORDER_FIELD(out[0].orders[0], side) == SIDE::BUY);
    EXPECT_EQ(ORDER_FIELD(out[0].orders[0], quantity), 5u);
    EXPECT_EQ(ORDER_FIELD(out[0].orders[0], price), 99);

    EXPECT_EQ(ORDER_FIELD(out[0].orders[1], order_id), 12u);
    EXPECT_TRUE(ORDER_FIELD(out[0].orders[1], side) == SIDE::SELL);
    EXPECT_EQ(ORDER_FIELD(out[0].orders[1], price), 101);
}

TEST(valid_empty_book) {
    // A symbol with no resting orders is a real state, not an error.
    Packet p;
    p.snapshot(SYM_A, 0, 0, 100);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::OK);
    EXPECT_EQ(out.size(), 1u);
    if (out.size() != 1) return;
    EXPECT_EQ(out[0].symbol, SYM_A);
    EXPECT_TRUE(out[0].orders.empty());
}

TEST(valid_two_snapshots_in_one_packet) {
    // Assumption kept from the original listener (outer while loop): one
    // packet may carry several symbols' snapshots. Unverified against spec.
    std::vector<Snapshot> out;
    EXPECT_EQ(parse(valid_two_symbol_packet(), out), SnapshotParseResult::OK);
    EXPECT_EQ(out.size(), 2u);
    if (out.size() != 2) return;

    EXPECT_EQ(out[0].symbol, SYM_A);
    EXPECT_EQ(out[0].last_md_seq_num, 100u);
    EXPECT_EQ(out[0].orders.size(), 2u);

    EXPECT_EQ(out[1].symbol, SYM_B);
    EXPECT_EQ(out[1].last_md_seq_num, 200u);
    EXPECT_EQ(out[1].orders.size(), 1u);
    if (out[1].orders.size() == 1) {
        EXPECT_EQ(ORDER_FIELD(out[1].orders[0], order_id), 21u);
    }
}

TEST(longer_header_length_is_skipped) {
    // header.length says 4 bytes more than we know about; those bytes exist.
    // Decision: skip them, so a protocol that appends fields doesn't break us.
    Packet p;
    p.snapshot(SYM_A, 1, 0, 100, SNAP_LEN + 4)
     .padding(4)
         .order(11, SYM_A, SIDE::BUY, 5, 99);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::OK);
    EXPECT_EQ(out.size(), 1u);
    if (out.size() == 1 && out[0].orders.size() == 1) {
        EXPECT_EQ(ORDER_FIELD(out[0].orders[0], order_id), 11u);
    } else {
        EXPECT_TRUE(false && "expected exactly one snapshot with one order");
    }
}

// ── Not enough bytes ───────────────────────────────────────────────────────

TEST(empty_packet) {
    std::vector<Snapshot> out;
    EXPECT_EQ(parse(Packet{}, out), SnapshotParseResult::NOT_ENOUGH_BYTES);
    EXPECT_TRUE(out.empty());
}

TEST(packet_shorter_than_md_header) {
    Packet p;
    p.snapshot(SYM_A, 0, 0, 100).truncate(sizeof(md_header) - 1);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::NOT_ENOUGH_BYTES);
    EXPECT_TRUE(out.empty());
}

TEST(packet_shorter_than_snapshot_info) {
    // A complete md_header, but the snapshot fields after it are cut off.
    Packet p;
    p.snapshot(SYM_A, 0, 0, 100).truncate(SNAP_LEN - 1);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::NOT_ENOUGH_BYTES);
    EXPECT_TRUE(out.empty());
}

TEST(snapshot_length_points_past_end_of_packet) {
    Packet p;
    p.snapshot(SYM_A, 0, 0, 100, /*length=*/0xFFFF);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::NOT_ENOUGH_BYTES);
    EXPECT_TRUE(out.empty());
}

TEST(counts_promise_more_orders_than_present) {
    // 3 bids promised, 2 delivered, then the packet ends.
    Packet p;
    p.snapshot(SYM_A, 3, 0, 100)
         .order(11, SYM_A, SIDE::BUY, 5, 99)
         .order(12, SYM_A, SIDE::BUY, 5, 98);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::NOT_ENOUGH_BYTES);
    EXPECT_TRUE(out.empty());
}

TEST(order_counts_overflow_uint32) {
    // bid_count + ask_count in uint32_t arithmetic wraps to 0, which would
    // accept a snapshot claiming ~4 billion orders as an empty book. The sum
    // must be computed in a wider type; then the very first order is missing.
    Packet p;
    p.snapshot(SYM_A, /*bids=*/0xFFFFFFFFu, /*asks=*/1, 100);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::NOT_ENOUGH_BYTES);
    EXPECT_TRUE(out.empty());
}

TEST(order_cut_off_mid_struct) {
    Packet p;
    p.snapshot(SYM_A, 1, 0, 100)
         .order(11, SYM_A, SIDE::BUY, 5, 99)
     .truncate(SNAP_LEN + ORDER_LEN - 3);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::NOT_ENOUGH_BYTES);
    EXPECT_TRUE(out.empty());
}

TEST(trailing_bytes_after_valid_snapshot) {
    // Too few to be another message. Garbage at the end means we cannot be
    // sure we understood the packet, so the whole packet is rejected.
    Packet p;
    p.snapshot(SYM_A, 0, 0, 100).padding(3);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::NOT_ENOUGH_BYTES);
    EXPECT_TRUE(out.empty());
}

TEST(every_truncation_of_a_valid_packet_fails_cleanly) {
    // Brute force: cut a valid two-snapshot packet at every byte offset. Each
    // prefix must be rejected with `out` empty -- and, under ASan, must not
    // read past its end.
    //
    // One exception: the cut that lands exactly after snapshot A's last order.
    // That prefix is a complete, valid one-snapshot packet, so it must parse.
    const Packet full = valid_two_symbol_packet();
    const size_t n = full.bytes().size();
    const size_t end_of_a = SNAP_LEN + 2 * ORDER_LEN;

    {
        Packet p = full;
        p.truncate(end_of_a);
        std::vector<Snapshot> out;
        EXPECT_EQ(parse(p, out), SnapshotParseResult::OK);
        EXPECT_EQ(out.size(), 1u);
    }

    size_t bad = 0;
    size_t first_bad = 0;
    for (size_t len = 0; len < n; ++len) {
        if (len == end_of_a) continue;
        Packet p = full;
        p.truncate(len);

        std::vector<Snapshot> out;
        const SnapshotParseResult r = parse(p, out);
        if (r == SnapshotParseResult::OK || !out.empty()) {
            if (bad == 0) first_bad = len;
            ++bad;
        }
    }
    if (bad != 0) {
        ::testing::record_failure(__FILE__, __LINE__,
            std::to_string(bad) + " of " + std::to_string(n) + " prefixes were accepted or "
            "left data in `out` (first: length " + std::to_string(first_bad) + ")");
    }
}

// ── Invalid header length ──────────────────────────────────────────────────

TEST(snapshot_length_zero) {
    // THE original bug: offset += 0 never advances, so the old loop spun
    // forever. If this test hangs, ctest's TIMEOUT kills it.
    Packet p;
    p.snapshot(SYM_A, 0, 0, 100, /*length=*/0);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::INVALID_HEADER_LENGTH);
    EXPECT_TRUE(out.empty());
}

TEST(snapshot_length_smaller_than_struct) {
    // Nonzero, so the loop advances -- but into the middle of this message's
    // own fields, which would then be misread as the next message.
    Packet p;
    p.snapshot(SYM_A, 0, 0, 100, /*length=*/sizeof(md_header));

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::INVALID_HEADER_LENGTH);
    EXPECT_TRUE(out.empty());
}

TEST(order_length_zero) {
    Packet p;
    p.snapshot(SYM_A, 1, 0, 100)
         .order(11, SYM_A, SIDE::BUY, 5, 99, /*length=*/0);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::INVALID_HEADER_LENGTH);
    EXPECT_TRUE(out.empty());
}

TEST(order_length_smaller_than_struct) {
    Packet p;
    p.snapshot(SYM_A, 1, 0, 100)
         .order(11, SYM_A, SIDE::BUY, 5, 99, /*length=*/ORDER_LEN - 1);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::INVALID_HEADER_LENGTH);
    EXPECT_TRUE(out.empty());
}

// ── Unknown message type ───────────────────────────────────────────────────

TEST(packet_does_not_start_with_snapshot_info) {
    Packet p;
    p.order(11, SYM_A, SIDE::BUY, 5, 99);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::UNKNOWN_MESSAGE_TYPE);
    EXPECT_TRUE(out.empty());
}

TEST(wrong_type_where_order_expected) {
    // Counts promise 2 orders; the second slot holds a DELETE_ORDER-typed
    // message. The original code would `break` here and still mark the
    // snapshot as received, leaving a half-built book flagged as good.
    Packet p;
    p.snapshot(SYM_A, 2, 0, 100)
         .order(11, SYM_A, SIDE::BUY, 5, 99)
         .order(12, SYM_A, SIDE::BUY, 5, 98, ORDER_LEN, MSG_TYPE::DELETE_ORDER);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::UNKNOWN_MESSAGE_TYPE);
    EXPECT_TRUE(out.empty());
}

TEST(next_snapshot_arrives_before_promised_orders) {
    // A has 2 bids promised but only 1 arrives before B's snapshot_info.
    // B carries an order so enough bytes follow for a whole new_order; that
    // way the parser gets past its size check and must notice the TYPE is
    // wrong. (Without B's order this would be NOT_ENOUGH_BYTES, which tests
    // check order rather than the broken promise.)
    Packet p;
    p.snapshot(SYM_A, 2, 0, 100)
         .order(11, SYM_A, SIDE::BUY, 5, 99)
     .snapshot(SYM_B, 1, 0, 200)
         .order(21, SYM_B, SIDE::BUY, 5, 50);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::UNKNOWN_MESSAGE_TYPE);
    EXPECT_TRUE(out.empty());
}

// ── All or nothing ─────────────────────────────────────────────────────────

TEST(bad_second_snapshot_discards_good_first) {
    // A is perfect; B is malformed. Returning A alone would mean the caller
    // applies half a packet. The contract says: error, and `out` empty.
    Packet p;
    p.snapshot(SYM_A, 1, 0, 100)
         .order(11, SYM_A, SIDE::BUY, 5, 99)
     .snapshot(SYM_B, 0, 0, 200, /*length=*/0);

    std::vector<Snapshot> out;
    EXPECT_EQ(parse(p, out), SnapshotParseResult::INVALID_HEADER_LENGTH);
    EXPECT_TRUE(out.empty());
}
