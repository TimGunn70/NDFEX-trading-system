# NDFEX Trading System

[![ci](https://github.com/TimGunn70/NDFEX-trading-system/actions/workflows/ci.yml/badge.svg)](https://github.com/TimGunn70/NDFEX-trading-system/actions/workflows/ci.yml)


A C++17 trading bot for a simulated exchange, built for a university trading competition,
then audited and rebuilt after it underperformed. 26 bug findings have been documented, and each
fix is backed by a test that originally failed. A multithreaded redesign is up next. 

> **Status:** the code builds and every fix has a test, but the strategy is
> turned off on purpose (see [Roadmap](#roadmap)). This repo is a record of
> auditing and fixing a system, not a bot that makes money.

---

## The story

### 1. The competition

The bot was originally built for a trading competition hosted by the High Frequency Trading Technologies
course at Notre Dame. During the competition, market data was broadcast via UDP multicast, and orders were sent using TCP. For our strategy, our bot attempted to market make on the GOLD/BLUE markets while simultaneously
attempting create/redeem arbitrage on the ETF and its underlyings. The arbitrage strategy ran for about half
of the competition, but it was losing money so we turned it off, and only the market making strategy ran
for the rest of the competition. After the competition, the course instructor confirmed that the exchange's bots were designed to punish those who attempted the arbitrage strategy. However, the audit found bugs (C2-C4) in our bot that would have caused it to lose money regardless of the 
bots' strategy. 

### 2. The audit

After the competition I asked Claude to flag issues throughout the codebase. The full list is in [docs/AUDIT.md](docs/AUDIT.md).

| Category | Findings | Fixed |
|---|---|---|
| Showstoppers (S) | 3 | 2 |
| Correctness, money-losing (C) | 7 | 1 |
| Market maker (MM) | 5 | 0 |
| Protocol / robustness (P) | 6 | 3 |
| Smaller issues (M) | 5 | 1 |

- **C1 (short-position PnL):** the average entry price only updated on buys, so selling from flat left it at 0, and buying back booked a large phantom loss, enough to make a short position look catastrophic.
- **P6 (unchecked market data):** the listener read each message struct without checking the packet was long enough, so a short UDP datagram left stale bytes from the previous packet in the reused buffer. The order book was then updated with prices that never existed, with no crash and nothing for AddressSanitizer to catch.

### 3. The rework

Every fix followed the same steps: write a test that fails on the old code,
commit it, then fix the code until the test passes. The history shows each
test commit before its fix.

| ID | What was wrong | What changed | Test |
|---|---|---|---|
| C1 | Short-position PnL | Separate average-entry logic for buys and sells | `test_symbol_manager` |
| P2 | Wire lengths trusted (OE framing, snapshots) | Bounds and length checks before every read | `test_oe_framing`, `test_snapshot_parser` |
| P6 | MD messages read without size checks | `validate_packet` before dispatch | `test_md_packet_validator` |
| P3 | `send()` result ignored, SIGPIPE | `send_all` loop, SIGPIPE disabled on both platforms | `test_send_all` |
| M4 | `exit(1)` in library code, silent listener failure | Errors returned; the bot shuts down if market data can't start | `test_multicast_socket`, `test_listener_startup` |
| S2 | Repo didn't build | CMake, plus CI on Linux and macOS | CI |
| S3 | Password committed | Read from an env var; real config stays local | — |

87 tests across 8 suites, run under AddressSanitizer and UBSan in CI.

Originally, the market data listener used epoll(), which only runs on Linux. I am doing the 
rework on a MacBook, so I had to learn how to use Docker and create a container image of Linux 
in order to build and test the bot. Now, the code uses poll() which is more 
portable, so it builds on both macOS and Linux. Furthermore, since there are only two sockets,
epoll's advantage of scaling to thousands of file descriptors is not applicable. Docker is still relevant 
for anything related to running/testing Linux on my personal machine. 

---

## Architecture

The bot currently runs two threads: one that tracks market data, and the other that keeps track of PnL. Order
entry has no thread of its own. Exchange replies are only read when the code is blocked in wait_for_response(),
so fills can go unread and the position goes stale (C4 in AUDIT.md). Eventually, the design will contain 
three threads: one for market data, one for strategy, and an order entry reader that processes fills, with each 
piece of the current state owned by a single thread. 


---

## Build and test

Requires a C++17 compiler and CMake 3.16 or newer. Linux is the supported
target, and CI also runs on macOS.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests don't need network access or an exchange.

## Running the bot

The bot needs a live NDFEX exchange (the course's simulated exchange) to
connect to.

```sh
cp config/bot.example.toml config/bot.toml   # then fill in the real addresses
export NDFEX_PASSWORD=...                    # never stored in the config file
./build/bot                                  # run from the repo root
```

---

## Project layout

```
src/common/    config, wire message definitions
src/md/        market data: listener, packet validation, order books
src/oe/        order entry: framing, OE client, ETF service client
src/strategy/  ETF arbitrage (currently disabled, see S1)
tests/         unit and malformed-input tests, plus fixtures
docs/          AUDIT.md
```

---

## Roadmap

The remaining findings, grouped in the order I plan to fix them:

1. **Threading rework (C4, M1, M2):** separate threads for market data,
   strategy, and order-entry reads, with single-writer ownership of state.
2. **Order state machine (C2, C3, C5, C6, C7):** an ACK is not a fill,
   timeouts on every request, IOC on arb legs, and separating a dead
   connection from a reject.
3. **Market maker rewrite (MM1–MM5).**
4. **Robustness (P1, P4, P5):** sequence-gap detection, clean shutdown
   that cancels open orders, and position reconciliation.
5. **Proof:** TSan-clean stress tests and a local sim-exchange harness.

The strategy is currently turned off intentionally (S1). The bot lost money live,
and bugs C2-C4 mean that any results it produces now are not trustworthy since ACK gets counted as a fill, 
and fills are only read while the bot is blocked. The strategy will be turned on once the order state machine
roadmap step is complete, so that its results can be trusted.

---

## How I used AI on this project

During this rework, I had Claude audit my entire codebase and document any bugs it found.
Then, rather than having it fix those bugs for me I used it to teach me how to fix them myself.
Even though I wrote most of the core logic for the fixes, I had Claude write the CMake build files,
CI, tests and logging. This work split was determined before I began the project rework.