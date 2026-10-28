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
FrameReadResult read_frame(int fd, char* buf, size_t buf_cap, size_t& out_len);
