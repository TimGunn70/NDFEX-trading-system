#pragma once

#include <cstddef>

enum class [[nodiscard]] FrameReadResult {
    OK,
    LENGTH_BELOW_HEADER,
    LENGTH_EXCEEDS_BUFFER,
    RECV_FAILED,
    SERVER_CLOSED_CONNECTION,
    CONNECTION_INTERRUPTED
};

const char* to_string(FrameReadResult result);

// On OK, buf[0, out_len) holds the complete frame including the header, and out_len == hdr.length.
// On any other result, buf and out_len are unspecified. The connection is unusable after any failure.
FrameReadResult read_frame(int fd, char* buf, size_t buf_cap, size_t& out_len);
