#include "test_main.h"

#include "multicast_socket.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

// AUDIT.md M4 — create_multicast_socket must report failure to its caller
// instead of calling exit(1), and must not leak the fd when it fails.
//
// Contract (see multicast_socket.h): valid fd on success, -1 on any failure,
// never terminates the process.
//
// How failures are forced, portably and without root:
//   - local_ip 192.0.2.1 is TEST-NET-1 (RFC 5737), reserved for docs, so no
//     machine has it on an interface. Joining a group there fails (the errno
//     varies by platform — ENODEV or EADDRNOTAVAIL — so tests check only -1).
//   - 127.0.0.1 is not a multicast group, so the join fails with EINVAL.
// Port 0 lets the kernel pick a free port, so tests never collide with each
// other or with a running bot.
//
// Against the old code, the first failure test calls exit(1) and kills this
// binary mid-run: ctest reports the failure as a non-zero exit, not a FAIL line.

namespace {
constexpr const char* kGroup        = "239.255.0.1";  // admin-scoped multicast
constexpr const char* kLoopback     = "127.0.0.1";    // always present
constexpr const char* kNoSuchIface  = "192.0.2.1";
constexpr const char* kNotMulticast = "127.0.0.1";

// POSIX: socket() returns the lowest unused fd. If a call leaks an fd, the
// next socket() gets a higher number than it would have.
int next_fd_number() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    close(fd);
    return fd;
}
}  // namespace

// Runs first so the happy path is reported before the old code's exit(1).
// Joins on loopback rather than 0.0.0.0: "any interface" needs a multicast
// route, which network-less containers and VMs don't have (ENODEV).
TEST(success_returns_open_nonblocking_fd) {
    int fd = create_multicast_socket(kGroup, 0, kLoopback);
    EXPECT_TRUE(fd >= 0);
    if (fd >= 0) {
        EXPECT_TRUE((fcntl(fd, F_GETFL, 0) & O_NONBLOCK) != 0);
        close(fd);
    }
}

TEST(join_on_unassigned_interface_returns_minus_one) {
    EXPECT_EQ(create_multicast_socket(kGroup, 0, kNoSuchIface), -1);
}

TEST(join_non_multicast_group_returns_minus_one) {
    EXPECT_EQ(create_multicast_socket(kNotMulticast, 0, kLoopback), -1);
}

TEST(failure_does_not_leak_fd) {
    int before = next_fd_number();
    EXPECT_EQ(create_multicast_socket(kGroup, 0, kNoSuchIface), -1);
    EXPECT_EQ(next_fd_number(), before);
}

TEST(repeated_failures_do_not_leak_fds) {
    int before = next_fd_number();
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(create_multicast_socket(kGroup, 0, kNoSuchIface), -1);
    }
    EXPECT_EQ(next_fd_number(), before);
}
