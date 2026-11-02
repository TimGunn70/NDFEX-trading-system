#include "test_main.h"

#include "config.h"
#include "listener.h"
#include "symbol_manager.h"

#include <atomic>
#include <sys/socket.h>
#include <unistd.h>

// AUDIT.md M4 (second half) — "a listener error returns silently and the bot
// runs on with no market data."
//
// Contract (listener.h): run_listener returns on fatal error and sets
// global_shutdown before returning. At startup that means: if either feed
// socket can't be created, return promptly, set the flag, and close any
// socket that WAS created.
//
// Failures are forced the same way as test_multicast_socket:
//   192.0.2.1 (TEST-NET-1) is on no interface -> the first join fails.
//   127.0.0.1 as a group is not multicast     -> that feed's join fails,
//     while the other feed joins fine on loopback.
// If run_listener ever falls through into its poll loop on failure, these
// tests hang; ctest's TIMEOUT turns that into a failure.

namespace {
// POSIX: socket() returns the lowest unused fd, so a leak raises this number.
int next_fd_number() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    close(fd);
    return fd;
}

Config::MarketData md_with(const char* live, const char* replay, const char* iface) {
    Config::MarketData md;
    md.live_addr   = live;
    md.replay_addr = replay;
    md.local_iface = iface;
    md.port        = 0;  // kernel picks; never collides with a running bot
    return md;
}
}  // namespace

// Live socket is created first, so it is the first to fail.
TEST(live_feed_failure_sets_shutdown_and_returns) {
    SymbolManager sm;
    std::atomic<bool> shutdown{false};
    int before = next_fd_number();

    run_listener(sm, md_with("239.255.0.1", "239.255.0.2", "192.0.2.1"), shutdown);

    EXPECT_TRUE(shutdown.load());
    EXPECT_EQ(next_fd_number(), before);
}

// Live succeeds, replay fails: the live socket must not be leaked.
TEST(replay_feed_failure_closes_live_socket) {
    SymbolManager sm;
    std::atomic<bool> shutdown{false};
    int before = next_fd_number();

    run_listener(sm, md_with("239.255.0.1", "127.0.0.1", "127.0.0.1"), shutdown);

    EXPECT_TRUE(shutdown.load());
    EXPECT_EQ(next_fd_number(), before);
}
