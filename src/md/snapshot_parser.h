#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "messages.h"

enum class SnapshotParseResult {
    OK,
    NOT_ENOUGH_BYTES,
    INVALID_HEADER_LENGTH,
    UNKNOWN_MESSAGE_TYPE
};

struct Snapshot {
    uint32_t symbol{};
    uint32_t last_md_seq_num{};
    std::vector<new_order> orders;
};

// If the result is OK, out holds every snapshot in the packet. Otherwise, out is empty.
SnapshotParseResult parse_snapshot(const char* buffer, size_t bytes, std::vector<Snapshot>& out);

