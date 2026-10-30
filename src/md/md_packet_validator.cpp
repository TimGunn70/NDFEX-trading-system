#include "md_packet_validator.h"
#include "messages.h"

#include <cstddef>
#include <cstring>

const char* to_string(MdPacketResult result) {
    switch (result) {
        case MdPacketResult::OK: return "OK";
        case MdPacketResult::TOO_SHORT_FOR_HEADER: return "TOO_SHORT_FOR_HEADER";
        case MdPacketResult::BAD_MAGIC_NUM: return "BAD_MAGIC_NUM";
        case MdPacketResult::UNKNOWN_MSG_TYPE: return "UNKNOWN_MSG_TYPE";
        case MdPacketResult::TOO_SHORT_FOR_MSG_TYPE: return "TOO_SHORT_FOR_MSG_TYPE";
    }
    return "UNKNOWN_VALIDATION_RESULT";
}

bool has_md_header(size_t bytes) {
   return bytes >= sizeof(md_header);
}

MdPacketResult validate_packet(const char* buf, size_t bytes) {
    if (!has_md_header(bytes)) {
        return MdPacketResult::TOO_SHORT_FOR_HEADER;
    }

    md_header h;

    // Use memcpy instead of reinterpret_cast - avoids strict-aliasing UB, and compiles to a plain load
    memcpy(&h, buf, sizeof(md_header));

    if (h.magic_number != MAGIC_NUMBER) {
        return MdPacketResult::BAD_MAGIC_NUM;
    }

    // 0 = no case matched. Every valid type needs at least sizeof(md_header) bytes, so can’t collide with a real size.
    size_t req_size = 0;

    switch(h.msg_type) {
        case MSG_TYPE::HEARTBEAT: req_size = sizeof(md_header); break;
        case MSG_TYPE::NEW_ORDER: req_size = sizeof(new_order); break;
        case MSG_TYPE::DELETE_ORDER: req_size = sizeof(delete_order); break;
        case MSG_TYPE::MODIFY_ORDER: req_size = sizeof(modify_order); break;
        case MSG_TYPE::TRADE: req_size = sizeof(trade); break;
        case MSG_TYPE::TRADE_SUMMARY: req_size = sizeof(trade_summary); break;
        case MSG_TYPE::SNAPSHOT_INFO: return MdPacketResult::UNKNOWN_MSG_TYPE;
    }

    if (req_size == 0) {
        return MdPacketResult::UNKNOWN_MSG_TYPE;
    }

    // <, not != so trailing bytes are ignored
    if (bytes < req_size) {
        return MdPacketResult::TOO_SHORT_FOR_MSG_TYPE;
    }

    return MdPacketResult::OK;
}

