#include "test_main.h"

#include "md_packet_validator.h"
#include "messages.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>
#include <vector>

// AUDIT.md P6 — market-data packets must be size-checked before any struct
// is read out of them.
//
// Interface under test (see md_packet_validator.h):
//   bool           has_md_header(size_t bytes);
//   MdPacketResult validate_packet(const char* buf, size_t bytes);
//   const char*    to_string(MdPacketResult);
//
// Contract these tests encode:
//   has_md_header  -> true iff bytes >= sizeof(md_header). The listener calls
//                     it before reading magic_number to route snapshots.
//   validate_packet validates NORMAL market-data packets only, and is safe on
//                   its own (it does not trust that has_md_header ran):
//     bytes < sizeof(md_header)                     -> TOO_SHORT_FOR_HEADER
//     magic != MAGIC_NUMBER (snapshot magic included;
//       snapshots go to parse_snapshot instead)     -> BAD_MAGIC_NUM
//     msg_type not a normal-feed type               -> UNKNOWN_MSG_TYPE
//       (SNAPSHOT_INFO counts as unknown here: it only
//        appears inside snapshot packets)
//     bytes < sizeof(<struct for msg_type>)         -> TOO_SHORT_FOR_MSG_TYPE
//       (HEARTBEAT is header-only: sizeof(md_header))
//     otherwise                                     -> OK
//   Trailing bytes past the struct are accepted (same choice as the snapshot
//   parser). hdr.length is NOT checked yet — see the note at the bottom.
//
// OK means: the caller may reinterpret buf as the struct named by msg_type.

static std::ostream& operator<<(std::ostream& os, MdPacketResult r) {
    return os << to_string(r);
}

// ── Wire layout pins ──────────────────────────────────────────────────────
// The validator is only as correct as sizeof(). These sizes are the current
// packed layout. If one fails, a struct changed (or lost __attribute__((packed)))
// and every size check in the listener changed with it — compare against the
// exchange's protocol spec before "fixing" the number here.
static_assert(sizeof(md_header)     == 23, "md_header wire size");
static_assert(sizeof(new_order)     == 45, "new_order wire size");
static_assert(sizeof(delete_order)  == 31, "delete_order wire size");
static_assert(sizeof(modify_order)  == 40, "modify_order wire size");
static_assert(sizeof(trade)         == 39, "trade wire size");
static_assert(sizeof(trade_summary) == 36, "trade_summary wire size");

namespace {

// A packet of `size` bytes whose header says magic/type. Bytes past the
// header are filler. Built in a vector, then validated from an exact-size
// heap copy so ASan reports any read past `size` (a vector's capacity may
// exceed its size and hide a one-byte overread).
std::vector<char> packet(size_t size, MSG_TYPE type,
                         uint64_t magic = MAGIC_NUMBER) {
    md_header h{};
    h.magic_number = magic;
    h.length       = static_cast<uint16_t>(size);
    h.msg_type     = type;

    std::vector<char> bytes(size < sizeof h ? sizeof h : size, '\xAB');
    std::memcpy(bytes.data(), &h, sizeof h);
    bytes.resize(size);  // may cut the header itself short
    return bytes;
}

MdPacketResult validate(const std::vector<char>& b) {
    std::unique_ptr<char[]> exact(new char[b.empty() ? 1 : b.size()]);
    if (!b.empty()) std::memcpy(exact.get(), b.data(), b.size());
    return validate_packet(exact.get(), b.size());
}

struct TypeSize {
    MSG_TYPE type;
    size_t   size;
    const char* name;
};

const TypeSize NORMAL_TYPES[] = {
    {MSG_TYPE::HEARTBEAT,     sizeof(md_header),     "HEARTBEAT"},
    {MSG_TYPE::NEW_ORDER,     sizeof(new_order),     "NEW_ORDER"},
    {MSG_TYPE::DELETE_ORDER,  sizeof(delete_order),  "DELETE_ORDER"},
    {MSG_TYPE::MODIFY_ORDER,  sizeof(modify_order),  "MODIFY_ORDER"},
    {MSG_TYPE::TRADE,         sizeof(trade),         "TRADE"},
    {MSG_TYPE::TRADE_SUMMARY, sizeof(trade_summary), "TRADE_SUMMARY"},
};

}  // namespace

// ── has_md_header ─────────────────────────────────────────────────────────

TEST(has_md_header_boundary) {
    EXPECT_FALSE(has_md_header(0));
    EXPECT_FALSE(has_md_header(1));
    EXPECT_FALSE(has_md_header(sizeof(md_header) - 1));
    EXPECT_TRUE(has_md_header(sizeof(md_header)));
    EXPECT_TRUE(has_md_header(sizeof(md_header) + 1));
    EXPECT_TRUE(has_md_header(1500));
}

// ── Valid packets ─────────────────────────────────────────────────────────

TEST(every_normal_type_at_exact_size_is_ok) {
    for (const TypeSize& t : NORMAL_TYPES) {
        EXPECT_EQ(validate(packet(t.size, t.type)), MdPacketResult::OK);
    }
}

TEST(trailing_bytes_are_accepted) {
    for (const TypeSize& t : NORMAL_TYPES) {
        EXPECT_EQ(validate(packet(t.size + 7, t.type)), MdPacketResult::OK);
    }
}

TEST(full_mtu_buffer_is_ok) {
    EXPECT_EQ(validate(packet(1500, MSG_TYPE::NEW_ORDER)), MdPacketResult::OK);
}

// ── Header too short ──────────────────────────────────────────────────────

TEST(empty_packet_is_too_short_for_header) {
    EXPECT_EQ(validate(packet(0, MSG_TYPE::NEW_ORDER)),
              MdPacketResult::TOO_SHORT_FOR_HEADER);
}

// Every length that cuts into the header. A validator that reads
// magic_number before checking `bytes` overreads here and ASan aborts.
TEST(every_truncation_inside_header_is_too_short_for_header) {
    for (size_t n = 0; n < sizeof(md_header); ++n) {
        EXPECT_EQ(validate(packet(n, MSG_TYPE::NEW_ORDER)),
                  MdPacketResult::TOO_SHORT_FOR_HEADER);
    }
}

// validate_packet must not rely on has_md_header having run first.
TEST(three_byte_datagram_does_not_read_magic) {
    EXPECT_EQ(validate(packet(3, MSG_TYPE::NEW_ORDER)),
              MdPacketResult::TOO_SHORT_FOR_HEADER);
}

// ── Magic ─────────────────────────────────────────────────────────────────

TEST(snapshot_magic_is_bad_magic_for_validate_packet) {
    EXPECT_EQ(validate(packet(sizeof(new_order), MSG_TYPE::NEW_ORDER,
                              SNAPSHOT_MAGIC_NUMBER)),
              MdPacketResult::BAD_MAGIC_NUM);
}

TEST(garbage_magic_is_bad_magic) {
    EXPECT_EQ(validate(packet(sizeof(new_order), MSG_TYPE::NEW_ORDER, 0)),
              MdPacketResult::BAD_MAGIC_NUM);
    EXPECT_EQ(validate(packet(sizeof(new_order), MSG_TYPE::NEW_ORDER,
                              0xDEADBEEFDEADBEEFull)),
              MdPacketResult::BAD_MAGIC_NUM);
}

// Magic is checked before type: a bad magic with a bad type reports the magic.
TEST(bad_magic_wins_over_unknown_type) {
    EXPECT_EQ(validate(packet(sizeof(new_order), static_cast<MSG_TYPE>(99), 0)),
              MdPacketResult::BAD_MAGIC_NUM);
}

// ── Message type ──────────────────────────────────────────────────────────

TEST(out_of_range_type_is_unknown) {
    EXPECT_EQ(validate(packet(64, static_cast<MSG_TYPE>(7))),
              MdPacketResult::UNKNOWN_MSG_TYPE);
    EXPECT_EQ(validate(packet(64, static_cast<MSG_TYPE>(255))),
              MdPacketResult::UNKNOWN_MSG_TYPE);
}

TEST(snapshot_info_type_on_normal_feed_is_unknown) {
    EXPECT_EQ(validate(packet(sizeof(snapshot_info), MSG_TYPE::SNAPSHOT_INFO)),
              MdPacketResult::UNKNOWN_MSG_TYPE);
}

// ── Per-type size ─────────────────────────────────────────────────────────

TEST(one_byte_short_of_each_type_is_too_short_for_msg_type) {
    for (const TypeSize& t : NORMAL_TYPES) {
        if (t.size == sizeof(md_header)) continue;  // HEARTBEAT: covered above
        EXPECT_EQ(validate(packet(t.size - 1, t.type)),
                  MdPacketResult::TOO_SHORT_FOR_MSG_TYPE);
    }
}

// Every length from a full header up to one byte short of the struct.
// Catches a check against the wrong struct (e.g. one sizeof(new_order) check
// for every type) and off-by-ones at either end.
TEST(every_truncation_inside_body_is_too_short_for_msg_type) {
    for (const TypeSize& t : NORMAL_TYPES) {
        for (size_t n = sizeof(md_header); n < t.size; ++n) {
            EXPECT_EQ(validate(packet(n, t.type)),
                      MdPacketResult::TOO_SHORT_FOR_MSG_TYPE);
        }
    }
}

// A header-only packet is a valid HEARTBEAT but a truncated NEW_ORDER.
TEST(header_only_packet_depends_on_type) {
    EXPECT_EQ(validate(packet(sizeof(md_header), MSG_TYPE::HEARTBEAT)),
              MdPacketResult::OK);
    EXPECT_EQ(validate(packet(sizeof(md_header), MSG_TYPE::DELETE_ORDER)),
              MdPacketResult::TOO_SHORT_FOR_MSG_TYPE);
}

// ── to_string ─────────────────────────────────────────────────────────────

TEST(to_string_names_every_value) {
    const MdPacketResult all[] = {
        MdPacketResult::OK,
        MdPacketResult::TOO_SHORT_FOR_HEADER,
        MdPacketResult::BAD_MAGIC_NUM,
        MdPacketResult::UNKNOWN_MSG_TYPE,
        MdPacketResult::TOO_SHORT_FOR_MSG_TYPE,
    };
    for (MdPacketResult r : all) {
        EXPECT_TRUE(to_string(r) != nullptr);
        EXPECT_TRUE(std::strlen(to_string(r)) > 0);
    }
}

// Not covered yet, deliberately: hdr.length vs bytes and vs sizeof(type).
// The listener never reads hdr.length today, and whether one datagram can
// carry several messages is unverified (open note for ARCHITECTURE.md).
// Decide that first, then add tests here.
