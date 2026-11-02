#pragma once

// Opens a non-blocking UDP socket bound to `port` and joined to the multicast
// group `mcast_addr` on the interface whose address is `local_ip`.
//
// Contract (AUDIT.md M4):
//   success -> a valid fd (>= 0), owned by the caller.
//   failure -> -1. Every failure leads to the same caller action (no market
//              data, stop the bot), so there is one failure value; which step
//              failed, and why, goes to the log with strerror(errno).
//              No fd is leaked on failure.
//   Never terminates the process.
[[nodiscard]] int create_multicast_socket(const char* mcast_addr, int port, const char* local_ip);
