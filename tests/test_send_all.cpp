#include "test_main.h"

#include "oe_framing.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

// AUDIT.md P3 — send() result ignored.
//
// Contract under test (oe_framing.h):
//   bool disable_sigpipe(int fd);  once per socket; macOS sets SO_NOSIGPIPE
//   bool send_all(int fd, const void* buf, size_t len);
//     true  -> all len bytes sent, in order, retrying short writes and EINTR
//     false -> errno says why; a closed peer returns false (EPIPE) instead of
//              killing the process with SIGPIPE.
//
// Bug-for-bug baseline: the old send_raw made ONE send() call and ignored the
// result. Against that, the short-write test loses bytes and the closed-peer
// test is killed by SIGPIPE.
//
// Forcing short writes: a 1 ms interval timer raises SIGALRM, installed WITHOUT
// SA_RESTART, while send() is blocked on a full socket buffer and a slow
// reader drains it. POSIX: a send interrupted after moving some bytes returns
// that partial count; one interrupted before moving any returns -1/EINTR. The
// harness test below proves the setup really produces short writes, so the
// send_all test can't pass by accident.

namespace {

struct SocketPair {
    int writer = -1;
    int reader = -1;
    SocketPair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
            writer = fds[0];
            reader = fds[1];
        }
    }
    ~SocketPair() {
        if (writer >= 0) ::close(writer);
        if (reader >= 0) ::close(reader);
    }
    void close_reader() {
        if (reader >= 0) { ::close(reader); reader = -1; }
    }
};

std::vector<char> pattern(size_t n) {
    std::vector<char> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<char>((i * 31) % 251);
    return v;
}

// Drains `fd` slowly on its own thread so the writer's buffer fills up.
// SIGALRM is blocked here so the timer always interrupts the WRITER.
struct SlowReader {
    std::vector<char> received;
    std::thread t;
    explicit SlowReader(int fd) {
        t = std::thread([this, fd] {
            sigset_t s;
            sigemptyset(&s);
            sigaddset(&s, SIGALRM);
            pthread_sigmask(SIG_BLOCK, &s, nullptr);
            char chunk[16 * 1024];
            for (;;) {
                ssize_t r = ::read(fd, chunk, sizeof(chunk));
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) break;  // writer closed (done) or error
                received.insert(received.end(), chunk, chunk + r);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }
    void join() { t.join(); }
};

std::atomic<int> g_alarms{0};
void on_alarm(int) { g_alarms.fetch_add(1, std::memory_order_relaxed); }

// RAII: SIGALRM every 1 ms with no SA_RESTART; restored on destruction.
struct InterruptTimer {
    struct sigaction old_sa{};
    InterruptTimer() {
        struct sigaction sa{};
        sa.sa_handler = on_alarm;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;  // deliberately no SA_RESTART
        sigaction(SIGALRM, &sa, &old_sa);
        itimerval tv{{0, 1000}, {0, 1000}};
        setitimer(ITIMER_REAL, &tv, nullptr);
    }
    ~InterruptTimer() {
        itimerval off{};
        setitimer(ITIMER_REAL, &off, nullptr);
        sigaction(SIGALRM, &old_sa, nullptr);
    }
};

constexpr size_t kBig = 8 * 1024 * 1024;  // far larger than any socket buffer

}  // namespace

TEST(small_message_sent_completely) {
    SocketPair p;
    EXPECT_TRUE(disable_sigpipe(p.writer));
    auto msg = pattern(40);
    EXPECT_TRUE(send_all(p.writer, msg.data(), msg.size()));
    ::shutdown(p.writer, SHUT_WR);  // reader sees EOF instead of blocking forever

    std::vector<char> got(msg.size());
    size_t n = 0;
    while (n < got.size()) {
        ssize_t r = ::read(p.reader, got.data() + n, got.size() - n);
        if (r <= 0) break;
        n += static_cast<size_t>(r);
    }
    EXPECT_EQ(n, msg.size());
    EXPECT_TRUE(got == msg);
}

TEST(zero_length_is_success) {
    SocketPair p;
    EXPECT_TRUE(disable_sigpipe(p.writer));
    char dummy = 0;
    EXPECT_TRUE(send_all(p.writer, &dummy, 0));
}

// Harness self-check: under InterruptTimer, a single raw send() of kBig really
// does come back short. If this ever stops being true on some platform, the
// next test proves nothing, and this one says so.
TEST(harness_produces_short_writes) {
    SocketPair p;
    auto msg = pattern(kBig);
    SlowReader reader(p.reader);
    ssize_t sent;
    {
        InterruptTimer timer;
        sent = ::send(p.writer, msg.data(), msg.size(), 0);
    }
    EXPECT_TRUE(sent < static_cast<ssize_t>(msg.size()));
    ::shutdown(p.writer, SHUT_WR);
    reader.join();
}

TEST(short_writes_and_eintr_still_deliver_every_byte_in_order) {
    SocketPair p;
    EXPECT_TRUE(disable_sigpipe(p.writer));
    auto msg = pattern(kBig);
    SlowReader reader(p.reader);
    bool ok;
    {
        InterruptTimer timer;
        ok = send_all(p.writer, msg.data(), msg.size());
    }
    ::shutdown(p.writer, SHUT_WR);
    reader.join();

    EXPECT_TRUE(ok);
    EXPECT_EQ(reader.received.size(), msg.size());
    EXPECT_TRUE(reader.received == msg);
    EXPECT_TRUE(g_alarms.load() > 0);  // the timer really fired
}

// Old behavior: SIGPIPE kills this test binary here.
TEST(closed_peer_returns_false_with_epipe_instead_of_dying) {
    SocketPair p;
    EXPECT_TRUE(disable_sigpipe(p.writer));
    p.close_reader();
    auto msg = pattern(40);
    errno = 0;
    EXPECT_FALSE(send_all(p.writer, msg.data(), msg.size()));
    EXPECT_EQ(errno, EPIPE);
}
