#include "test_main.h"

#include "oe_framing.h"
#include "oe_messages.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>
#include <pthread.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// AUDIT.md P2 (second half) — OE response framing must not trust hdr.length.
//
// Contract under test (see oe_framing.h):
//   OK  -> buf[0, out_len) is one whole frame, header included;
//          out_len == hdr.length, and sizeof(header) <= out_len <= buf_cap.
//   any failure -> buf and out_len are unspecified. These tests check only
//          the result value; the caller closes the connection either way.
//
// Result mapping these tests encode:
//   hdr.length < sizeof(header)          -> LENGTH_BELOW_HEADER
//   hdr.length > buf_cap                 -> LENGTH_EXCEEDS_BUFFER, decided
//                                           from the header alone, before any
//                                           body byte is read
//   recv() == -1, errno != EINTR         -> RECV_FAILED
//   recv() == -1, errno == EINTR         -> retried, never reported
//   peer closes before byte 0 of a frame -> SERVER_CLOSED_CONNECTION
//   peer closes after byte 0 of a frame  -> CONNECTION_INTERRUPTED
//
// Harness: socketpair(AF_UNIX, SOCK_STREAM) gives a real byte stream with no
// network. The test writes into one end (playing the exchange) and read_frame
// reads the other. Every receive buffer is an exact-size heap block, so a
// write one byte past buf_cap is an ASan heap-buffer-overflow, not silence.

// Lets EXPECT_EQ print the enum by name (global namespace for ADL; see the
// matching note in test_snapshot_parser.cpp).
static std::ostream& operator<<(std::ostream& os, FrameReadResult r) {
    return os << to_string(r);
}

namespace {

using Header = ndfex::oe::oe_response_header;
constexpr size_t HDR = sizeof(Header);   // 16
constexpr size_t CAP = 256;              // wait_for_response's stack buffer

// Full-write loop: a single write() may write less than asked. (Yes, this is
// P3 in miniature — the test harness cannot afford the bug it will test for.)
void write_all(int fd, const char* p, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::testing::record_failure(__FILE__, __LINE__, "test harness write() failed");
            return;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
}

// RAII socketpair. `reader` is handed to read_frame; `writer` is the exchange.
struct Pipe {
    int reader = -1;
    int writer = -1;

    Pipe() {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            reader = sv[0];
            writer = sv[1];
        } else {
            ::testing::record_failure(__FILE__, __LINE__, "socketpair() failed");
        }
    }
    ~Pipe() {
        close_writer();
        if (reader >= 0) ::close(reader);
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    void send(const std::vector<char>& bytes) { write_all(writer, bytes.data(), bytes.size()); }
    void send(const std::vector<char>& bytes, size_t first_n) { write_all(writer, bytes.data(), first_n); }
    // The exchange hangs up: read_frame's next recv() past the data returns 0.
    void close_writer() {
        if (writer >= 0) {
            ::close(writer);
            writer = -1;
        }
    }
};

// A 16-byte response header with the given length field. Other fields are
// arbitrary but nonzero, so a byte-compare would catch them being dropped.
std::vector<char> header_bytes(uint16_t length) {
    Header h{};
    h.length       = length;
    h.msg_type     = static_cast<uint8_t>(ndfex::oe::MSG_TYPE::ACK);
    h.version      = ndfex::oe::OE_PROTOCOL_VERSION;
    h.seq_num      = 7;
    h.last_seq_num = 6;
    h.client_id    = 42;
    std::vector<char> out(HDR);
    std::memcpy(out.data(), &h, HDR);
    return out;
}

// Header claiming `length` followed by exactly length - 16 patterned body
// bytes. Requires length >= 16.
std::vector<char> frame_bytes(uint16_t length) {
    std::vector<char> out = header_bytes(length);
    for (size_t i = HDR; i < length; ++i) out.push_back(static_cast<char>((i * 31) ^ 0x5A));
    return out;
}

std::vector<char> junk(size_t n) { return std::vector<char>(n, '\x7E'); }

std::vector<char> concat(std::vector<char> a, const std::vector<char>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

struct Read {
    FrameReadResult result;
    size_t len;
    std::unique_ptr<char[]> buf;   // exactly `cap` bytes — ASan sees overruns
};

Read read_one(int fd, size_t cap = CAP) {
    Read r{FrameReadResult::OK, 0xDEADBEEF, std::unique_ptr<char[]>(new char[cap])};
    r.result = read_frame(fd, r.buf.get(), cap, r.len);
    return r;
}

// On OK, buf must hold exactly `expected`, header included.
void expect_frame(const Read& r, const std::vector<char>& expected, int line) {
    if (r.result != FrameReadResult::OK) {
        ::testing::record_failure(__FILE__, line, std::string("expected OK, got ") + to_string(r.result));
        return;
    }
    if (r.len != expected.size()) {
        ::testing::record_failure(__FILE__, line, "out_len " + std::to_string(r.len) +
                                  " != frame size " + std::to_string(expected.size()));
        return;
    }
    if (std::memcmp(r.buf.get(), expected.data(), expected.size()) != 0) {
        ::testing::record_failure(__FILE__, line, "frame bytes differ from what was sent");
    }
}

}  // namespace

// ---------------------------------------------------------------- valid frames

TEST(ok_header_only_frame) {
    // length == 16 is the smallest legal frame: header, empty body.
    Pipe p;
    auto f = frame_bytes(HDR);
    p.send(f);
    expect_frame(read_one(p.reader), f, __LINE__);
}

TEST(ok_ack_sized_frame_includes_header) {
    Pipe p;
    auto f = frame_bytes(sizeof(ndfex::oe::order_ack));
    p.send(f);
    expect_frame(read_one(p.reader), f, __LINE__);
}

TEST(ok_frame_exactly_fills_buffer) {
    Pipe p;
    auto f = frame_bytes(CAP);
    p.send(f);
    expect_frame(read_one(p.reader), f, __LINE__);
}

TEST(ok_back_to_back_frames_are_read_one_at_a_time) {
    // Both frames arrive in one write. read_frame must stop at the first
    // frame's end — reading "as much as fits" would eat the second one.
    Pipe p;
    auto a = frame_bytes(40);
    auto b = frame_bytes(24);
    p.send(concat(a, b));
    p.close_writer();
    expect_frame(read_one(p.reader), a, __LINE__);
    expect_frame(read_one(p.reader), b, __LINE__);
    EXPECT_EQ(read_one(p.reader).result, FrameReadResult::SERVER_CLOSED_CONNECTION);
}

TEST(ok_frame_dripped_one_byte_per_write) {
    // TCP has no message boundaries: one recv() may return any prefix.
    Pipe p;
    auto f = frame_bytes(sizeof(ndfex::oe::order_fill));
    int w = p.writer;
    std::thread exchange([w, &f] {
        for (char c : f) {
            write_all(w, &c, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    Read r = read_one(p.reader);
    exchange.join();
    expect_frame(r, f, __LINE__);
}

// ------------------------------------------------------------- invalid lengths

TEST(length_below_header_is_rejected) {
    // Old code: remaining = length - 16 wraps to ~2^64 and recv() writes the
    // trailing junk past the end of buf. The junk is there so that it would.
    for (uint16_t len : {0, 1, 8, 15}) {
        Pipe p;
        p.send(concat(header_bytes(len), junk(300)));
        Read r = read_one(p.reader);
        EXPECT_EQ(r.result, FrameReadResult::LENGTH_BELOW_HEADER);
    }
}

TEST(length_one_past_buffer_is_rejected) {
    // Old code: no check against the buffer, so the body is received straight
    // into memory past buf[CAP-1].
    Pipe p;
    p.send(frame_bytes(CAP + 1));
    EXPECT_EQ(read_one(p.reader).result, FrameReadResult::LENGTH_EXCEEDS_BUFFER);
}

TEST(length_exceeds_buffer_is_decided_from_header_alone) {
    // Only the header is sent and the writer stays OPEN. An implementation
    // that tries to read the body before validating the length will block
    // here forever — ctest's TIMEOUT turns that into a failure.
    Pipe p;
    p.send(header_bytes(0xFFFF));
    EXPECT_EQ(read_one(p.reader).result, FrameReadResult::LENGTH_EXCEEDS_BUFFER);
}

TEST(buffer_limit_comes_from_buf_cap_not_a_constant) {
    {
        Pipe p;
        auto f = frame_bytes(64);
        p.send(f);
        expect_frame(read_one(p.reader, 64), f, __LINE__);
    }
    {
        Pipe p;
        p.send(frame_bytes(65));
        EXPECT_EQ(read_one(p.reader, 64).result, FrameReadResult::LENGTH_EXCEEDS_BUFFER);
    }
}

// --------------------------------------------------------- connection endings

TEST(close_before_any_byte_is_server_closed) {
    Pipe p;
    p.close_writer();
    EXPECT_EQ(read_one(p.reader).result, FrameReadResult::SERVER_CLOSED_CONNECTION);
}

TEST(close_after_any_byte_is_interrupted_at_every_offset) {
    // Truncate a valid 40-byte frame at every offset: 1..15 cut the header,
    // 16..39 cut the body. Offset 0 is a clean close, not a truncation.
    auto f = frame_bytes(40);
    for (size_t k = 0; k < f.size(); ++k) {
        Pipe p;
        p.send(f, k);
        p.close_writer();
        FrameReadResult want = (k == 0) ? FrameReadResult::SERVER_CLOSED_CONNECTION
                                        : FrameReadResult::CONNECTION_INTERRUPTED;
        FrameReadResult got = read_one(p.reader).result;
        if (got != want) {
            ::testing::record_failure(__FILE__, __LINE__, "truncated at " + std::to_string(k) +
                                      ": got " + to_string(got) + ", expected " + to_string(want));
        }
    }
}

TEST(recv_error_is_recv_failed) {
    // recv() on an invalid descriptor fails with EBADF.
    EXPECT_EQ(read_one(-1).result, FrameReadResult::RECV_FAILED);
}

// ---------------------------------------------------------------------- EINTR
//
// A signal handler installed WITHOUT SA_RESTART makes a blocked recv() return
// -1 / EINTR. That is not an error: the right response is to call recv again.
// The helper thread waits until the main thread is (almost certainly) blocked
// in recv, signals it, then sends the bytes. `g_signals` proves the signal
// actually landed — if it is 0, the test was not exercising anything.

namespace {

volatile std::sig_atomic_t g_signals = 0;
void on_sigusr1(int) { g_signals = g_signals + 1; }

// Sends `before`, waits, interrupts the reader, waits, sends `after`.
Read read_with_interrupt(const std::vector<char>& before, const std::vector<char>& after) {
    struct sigaction sa{};
    struct sigaction old{};
    sa.sa_handler = on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                        // deliberately no SA_RESTART
    sigaction(SIGUSR1, &sa, &old);
    g_signals = 0;

    Pipe p;
    int w = p.writer;
    pthread_t reader_thread = pthread_self();
    std::thread exchange([&] {
        if (!before.empty()) write_all(w, before.data(), before.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pthread_kill(reader_thread, SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        write_all(w, after.data(), after.size());
    });
    Read r = read_one(p.reader);
    exchange.join();

    sigaction(SIGUSR1, &old, nullptr);
    return r;
}

}  // namespace

TEST(eintr_while_reading_header_is_retried) {
    auto f = frame_bytes(40);
    Read r = read_with_interrupt({}, f);
    EXPECT_TRUE(g_signals > 0);
    expect_frame(r, f, __LINE__);
}

TEST(eintr_while_reading_body_is_retried) {
    auto f = frame_bytes(40);
    std::vector<char> head(f.begin(), f.begin() + HDR);
    std::vector<char> body(f.begin() + HDR, f.end());
    Read r = read_with_interrupt(head, body);
    EXPECT_TRUE(g_signals > 0);
    expect_frame(r, f, __LINE__);
}
