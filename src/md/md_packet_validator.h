#pragma once

#include <cstddef>


enum class [[nodiscard]] MdPacketResult {
    OK,
    TOO_SHORT_FOR_HEADER,
    BAD_MAGIC_NUM,
    UNKNOWN_MSG_TYPE,
    TOO_SHORT_FOR_MSG_TYPE
};

const char* to_string(MdPacketResult result);

// Returns true iff bytes >= sizeof(md_header).
// Call this before reading magic_number to route snapshot vs normal packets. 
[[nodiscard]] bool has_md_header(size_t bytes);

// Validates a NORMAL market-data packet (magic == MAGIC_NUMBER).
// Snapshot packets are not handled here; route them to parse_snapshot().
// Safe to call on its own, does not assume has_md_header() was called.
//
// On OK: buf[0, bytes) holds a complete message struct for hdr.msg_type,
// so the caller may reinterpret buf as the struct matching hdr.msg_type. 
// Bytes past the struct are ignored. On anything else, the caller must reject the packet
// and buf is unchanged.
MdPacketResult validate_packet(const char* buf, size_t bytes);

