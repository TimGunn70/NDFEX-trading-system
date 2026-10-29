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

[[nodiscard]] bool has_md_header(size_t bytes);

MdPacketResult validate_packet(const char* buf, size_t bytes);

