#include "md_packet_validator.h"

#include <cstddef>

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
    return true;
}

MdPacketResult validate_packet(const char* buf, size_t bytes) {
    return MdPacketResult::OK;
}

