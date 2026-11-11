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

// Call this function once after creating the socket.
// Linux does not rely on this function since send_all uses MSG_NOSIGNAL
// On MacOS this sets SO_NOSIGPIPE, and returns false if the option couldn't be set
[[nodiscard]] bool disable_sigpipe(int fd);

// Sends all len bytes, retrying short writes and EINTR.
// True means all bytes were sent, false produces an errno.
// This function requires disable_sigpipe(fd) to succeed, so a closed peer returns false
// instead of killing the process.
// This function also assumes that the socket is blocking. On a non-blocking socket, EAGAIN would be reported as failure.
[[nodiscard]] bool send_all(int fd, const void* buf, size_t len);
