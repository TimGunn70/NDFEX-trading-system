#include "snapshot_parser.h"
#include "messages.h"

#include <cstring>
#include <utility>

SnapshotParseResult parse_snapshot(const char* buffer, size_t bytes, std::vector<Snapshot>& out) {
    out.clear();
    size_t offset = 0;

    // Return if packet is empty
    if (!bytes) {
        return SnapshotParseResult::NOT_ENOUGH_BYTES;
    }

    while (offset < bytes) {

        // Check if snapshot info is present
        if (sizeof(snapshot_info) > bytes - offset) {
            out.clear();
            return SnapshotParseResult::NOT_ENOUGH_BYTES;
        }

        // Copy the info into a local snapshot
        snapshot_info info;
        std::memcpy(&info, buffer + offset, sizeof(snapshot_info));

        // Check for the correct message type
        if (info.header.msg_type != MSG_TYPE::SNAPSHOT_INFO) {
            out.clear();
            return SnapshotParseResult::UNKNOWN_MESSAGE_TYPE;
        }

        // Check if header.length is valid
        const size_t len = info.header.length;

        if (len < sizeof(snapshot_info)) {
            out.clear();
            return SnapshotParseResult::INVALID_HEADER_LENGTH;
        }

        // Check if header.length fits in the remaining bytes
        if (len > bytes - offset) {
            out.clear();
            return SnapshotParseResult::NOT_ENOUGH_BYTES;
        }

        Snapshot snapshot{};
        snapshot.symbol = info.symbol;
        snapshot.last_md_seq_num = info.last_md_seq_num;
        offset += len;

        const size_t total = static_cast<size_t>(info.ask_count) + static_cast<size_t>(info.bid_count);
        for (size_t i = 0; i < total; i++) {
            // Check if a new_order fits
            if (sizeof(new_order) > bytes - offset) {
                out.clear();
                return SnapshotParseResult::NOT_ENOUGH_BYTES;
            }

            // Copy into a local new order
            new_order order;
            std::memcpy(&order, buffer + offset, sizeof(new_order));

            if (order.header.msg_type != MSG_TYPE::NEW_ORDER) {
                out.clear();
                return SnapshotParseResult::UNKNOWN_MESSAGE_TYPE;
            }

            const size_t order_len = order.header.length;

            if (order_len < sizeof(new_order)) {
                out.clear();
                return SnapshotParseResult::INVALID_HEADER_LENGTH;
            }

            if (order_len > bytes - offset) {
                out.clear();
                return SnapshotParseResult::NOT_ENOUGH_BYTES;
            }

            snapshot.orders.push_back(order);
            offset += order_len;
        }

        out.push_back(std::move(snapshot));
    }

    return SnapshotParseResult::OK;
}

const char* to_string(SnapshotParseResult r) {
    switch (r) {
        case SnapshotParseResult::OK:                    return "OK";
        case SnapshotParseResult::NOT_ENOUGH_BYTES:      return "NOT_ENOUGH_BYTES";
        case SnapshotParseResult::INVALID_HEADER_LENGTH: return "INVALID_HEADER_LENGTH";
        case SnapshotParseResult::UNKNOWN_MESSAGE_TYPE:  return "UNKNOWN_MESSAGE_TYPE";
    }
    return "UNKNOWN_RESULT";  // only reachable if someone casts a bad int into the enum
}
