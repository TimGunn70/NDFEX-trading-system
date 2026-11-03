
#include "oe_framing.h"
#include "oe_messages.h"


#include <cstring>
#include <cstddef>
#include <sys/socket.h>
#include <cerrno>



const char* to_string(FrameReadResult result) {
    switch (result) {
        case FrameReadResult::OK: return "OK";
        case FrameReadResult::LENGTH_BELOW_HEADER: return "LENGTH_BELOW_HEADER";
        case FrameReadResult::LENGTH_EXCEEDS_BUFFER: return "LENGTH_EXCEEDS_BUFFER";
        case FrameReadResult::RECV_FAILED: return "RECV_FAILED";
        case FrameReadResult::SERVER_CLOSED_CONNECTION: return "SERVER_CLOSED_CONNECTION";
        case FrameReadResult::CONNECTION_INTERRUPTED: return "CONNECTION_INTERRUPTED";
    }
    return "UNKNOWN_READ_RESULT";
}

FrameReadResult read_frame(int fd, char* buf, size_t buf_cap, size_t& out_len) {
    ndfex::oe::oe_response_header hdr;
    size_t total = 0;
    while (total < sizeof(hdr)) {
        ssize_t n = recv(fd, (char*)&hdr + total, sizeof(hdr) - total, 0);
        if (n < 0) { 
            if (errno == EINTR) continue;
            return FrameReadResult::RECV_FAILED;
        }
        if (n == 0) {
            if (total == 0) return FrameReadResult::SERVER_CLOSED_CONNECTION;
            else return FrameReadResult::CONNECTION_INTERRUPTED;
        }
        total += n;
    }

    if (hdr.length > buf_cap) {
        return FrameReadResult::LENGTH_EXCEEDS_BUFFER;
    }

    if (hdr.length < sizeof(hdr)) {
        return FrameReadResult::LENGTH_BELOW_HEADER;
    }

    memcpy(buf, &hdr, sizeof(hdr));
    size_t remaining = hdr.length - sizeof(hdr);
    total = 0;
    while (total < remaining) {
        ssize_t n = recv(fd, buf + sizeof(hdr) + total, remaining - total, 0);
        if (n < 0) { 
            if (errno == EINTR) continue;
            return FrameReadResult::RECV_FAILED;
        }
        if (n == 0) return FrameReadResult::CONNECTION_INTERRUPTED;
        total += n;
    }

    out_len = hdr.length;
    return FrameReadResult::OK;
}

bool disable_sigpipe(int fd) {
    // For Mac OS
    #ifdef SO_NOSIGPIPE
        int on = 1;
        return (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0);
    #else
        // No socket option needed since send_all passes MSG_NOSIGNAL on every call.
        (void)fd;
        return true;
    #endif
}

bool send_all(int fd, const void* buf, size_t len) {
    #ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
    #else
        const int flags = 0;
    #endif

    const char* ptr = static_cast<const char *>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, flags);

        if (sent == -1 && errno != EINTR) {
            return false;
        }
        else if (sent > 0) {
            ptr += sent;
            remaining -= static_cast<size_t>(sent);
        }
    }

    return true;
}

