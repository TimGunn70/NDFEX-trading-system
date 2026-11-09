> **History note:** This audit covers the final snapshot plus a recovered
> earlier main.cpp. During the competition the system ran a combined
> arb + market-making loop (`run_with_mm`) quoting GOLD and BLUE around a
> tick-rounded mid at ±1 tick, size 1, requoting only when the rounded mid
> moved. The ETF arb ran for roughly the first half of the competition and was then
> turned off manually after losing money. The instructor later confirmed the
> environment was designed to make arbitrage difficult; separately, findings
> C2–C4 would have lost money regardless. Some MM logic (notably asymmetric short-covering,
> MM1) was added by a teammate; its intent is unverified. Where authorship
> or rationale is unknown, findings say so — this document records what the
> code does, and hypotheses are labeled as hypotheses.

# Post-Competition Audit — Group 08 Trading System

Audit of the CSE 40438 ETF-arbitrage trading system, performed after the
competition ended. This document drives the rework: each finding gets fixed,
tested, and checked off. Findings are grouped by severity.

**System summary:** C++17 bot for the NDFEX simulated exchange. UDP multicast
market data (live + replay/snapshot feeds) → per-symbol order books →
ETF creation/redemption arbitrage across 10 underlyings and 1 ETF (UNDY),
with TCP order entry and an HTTP client for the ETF create/redeem service.

---

## 1. Showstoppers

- [ ] **S1 — Strategy never runs.** `ETFArb` is constructed in `main.cpp` but no
  thread or loop ever calls `try_creation_arb`/`try_redemption_arb`. The bot
  connects, logs in, and prints PnL forever.
  *Status: intentionally left unfixed. The arb lost money live; the instructor
  confirmed the environment was designed to make arbitrage difficult, and
  C2–C4 would lose money regardless. Re-enable after the order state machine
  (§5 step 5) so results can be measured.*
- [x] **S2 — Repo doesn't build.** Makefile references `position_tracker.cpp`,
  `pnl_tracker.cpp`, `risk_manager.cpp`, `test_risk.cpp`; `exposure_tracker.h`
  includes the missing `position_tracker.h`. `make bot` and `make tests` fail.
  *Fixed: Makefile replaced by CMake; CI builds and tests on Ubuntu (green at
  `12a3d73`). `exposure_tracker.{h,cpp}` are excluded from the build because
  `position_tracker.h` does not exist; kept as a design reference for §5 step 5.*
- [x] **S3 — Secrets committed.** Plaintext password in `main.cpp`; `oe_log.txt`
  (checked in) contains hex dumps of login messages including the password;
  compiled `bot` binary committed. Fix: fresh repo, config/env for credentials,
  `.gitignore` for logs and binaries.
  *Fixed: password read from `NDFEX_PASSWORD`, not in any commit; logs, binary,
  and build dirs gitignored; real config is local-only (`config/bot.toml`
  ignored, `bot.example.toml` tracked). Course server addresses remain in
  `40e5c99`. Accepted: no credentials, course infrastructure is intermittent,
  and the instructor approved publishing the project.*

## 2. Correctness bugs (money-losing)

- [x] **C1 — Short-position PnL is wrong.** `SymbolManager::on_fill` updates
  `avg_entry_price_` only on BUY. Selling from flat leaves avg entry = 0, so
  buying back books realized PnL of `(0 − price) × qty`. Position flips through
  zero also blend into a garbage average. Fix with per-side avg-entry logic and
  unit tests for: open long, add, reduce, close, open short, cover, flip.
- [ ] **C2 — ACK treated as fill.** Creation arb counts `wait_for_response()`
  successes as "legs filled," but it returns true on ACK (order *resting*) and
  on CLOSE. A resting unfilled leg counts as filled. Fills must come only from
  FILL messages; ACK only transitions PendingNew → Working.
- [ ] **C3 — Redemption arb races itself.** UNDY buy returns on ACK, then
  `/redeem` is called immediately — fails if unfilled, and the resting buy is
  left live (unhedged if it fills later). Needs fill confirmation before
  redeem, and cancel-on-abort.
- [ ] **C4 — Fills processed only while blocked in `wait_for_response()`.**
  No dedicated reader thread; fills sit in the socket buffer, positions go
  stale, and the "sell UNDY until flat" retry loop polls a position that can't
  update (over-sell risk). **This is the core motivation for the multithreaded
  re-architecture.**
- [ ] **C5 — Response-count mismatches hang forever.** `unwind_dorm_longs`
  skips symbols but still waits for exactly 10 responses; no timeouts anywhere
  in the OE path.
- [ ] **C6 — IOC never used.** All orders sent with `flags=0` though the
  protocol supports IOC. Arb legs should be IOC so failed arbs don't leave
  resting orders.
- [ ] **C7 — Dead OE connection is indistinguishable from a reject.** When
  `read_frame` fails, `read_response` (`oe_client.cpp`) logs the failure but
  doesn't close the socket or mark the client unusable. `wait_for_response`
  returns `false` both for this and for a REJECT, so callers treat a dead
  connection as a single rejected order and keep trading. Some callers
  (`etf_arb.cpp:171`, `:189`) ignore the return value entirely. From then on,
  no FILL or CLOSE messages are processed, so the bot's position goes stale
  while orders keep going out, and the next `send_raw` writes to a dead socket
  (SIGPIPE, see P3). Can be triggered by a malformed frame, not just a dropped
  connection (see P2). Fix: distinguish the failures in the return type; on a
  connection failure, close the fd and stop trading.

## 2b. Market-maker findings (from recovered pre-final main.cpp)

- [ ] **MM1 — Asymmetric panic short-covering.** While short, the MM lifts the
  ask for the full position every 100ms loop — paying the spread repeatedly.
  No equivalent for longs. Hypothesis: reaction to C1 (short PnL bug made
  shorts show phantom catastrophic losses). Verify against team memory.
- [ ] **MM2 — Blocking cancel-then-replace quoting.** Each requote is 4
  blocking round trips (cancel×2, place×2) on the strategy thread; quotes are
  absent from the book during the gap, and position reads mid-sequence race
  fills processed inside those same blocking calls (the "fresh read" comments
  are the workaround's fossil).
- [ ] **MM3 — Last-writer-wins fill callback.** OEClient has one on_fill slot;
  ETFArb's constructor silently overwrites the MM's callback, forcing a
  construction-order dependency documented only in a comment. Root cause of
  the merged `run_with_mm` design. Fix: route fills by order ID to registered
  handlers (or per-strategy queues in the threaded design).
- [ ] **MM4 — Unsafe signal handlers.** SIGSEGV/SIGABRT handlers call
  non-async-signal-safe functions; SIGINT saves positions but exits without
  canceling live orders (P4) and calls std::exit from a handler.
- [ ] **MM5 — Order-ID space partitioned by convention** (MM from 90000, arb
  from 1000) — collision is possible, nothing enforces it. Positions persisted
  to positions.txt with no exchange reconciliation on reload (ties to P5).

## 3. Protocol / robustness holes

- [ ] **P1 — No sequence-gap detection.** `md_header.seq_num` never checked;
  UDP drops/reorders silently corrupt books. `last_seq_num_` is stored but
  unused. Fix: gap detection + book invalidation + re-snapshot.
- [x] **P2 — Wire data trusted.** In `read_response`, `hdr.length <
  sizeof(hdr)` underflows `remaining` (size_t) → overflow of the 256-byte
  stack buffer; no check that `hdr.length ≤ buf`. Snapshot parser: `offset +=
  header.length` with length 0 loops forever; no bounds check vs packet size.
- [x] **P3 — `send()` result ignored.** Short write corrupts TCP framing for
  every later message; dead connection raises SIGPIPE and kills the process.
  Fix: full-write loop, `MSG_NOSIGNAL`, error propagation.
  *Fixed: `send_all` (`oe_framing.{h,cpp}`) retries short writes and EINTR and
  returns `false` with errno set on any other error; `disable_sigpipe` sets
  `SO_NOSIGPIPE` on macOS, and `send_all` passes `MSG_NOSIGNAL` on Linux, so a
  closed peer returns EPIPE instead of killing the process. Every send in the
  tree goes through it: `OEClient::send_raw` (now `[[nodiscard]] bool`; waiting
  callers stop before `wait_for_response` on failure) and the ETF HTTP request.
  Tests: `test_send_all` (closed peer; short writes forced by a SIGALRM timer,
  with a harness check that the setup really produces them). Remaining:
  `send_new_order_no_wait` can't report failure to its caller (`TODO(C7)`).*
- [ ] **P4 — No shutdown path.** No signal handler; `global_shutdown` never
  set; `cancel_all_open_orders()` never called. Ctrl-C leaves live orders on
  the exchange. No kill switch: PnL −4000 warns but keeps trading toward the
  −5000 floor.
- [ ] **P5 — Position drift undetected.** Fills for unknown order IDs are
  logged and ignored; `ETFClient::get_positions()` exists but is never used to
  reconcile local vs exchange state.
- [x] **P6 — MD messages read without size checks.** In `listener.cpp`
  (live path ~161/197–210, buffered drain ~103), `buf` is cast to
  `md_header*` and then to message structs without comparing `bytes` to
  the size of the struct being read. Because `buf` is a reused array that `read()`
  only partially overwrites, a short datagram leaves old data in the
  unread tail, so the book is silently corrupted (no crash, no ASan report).
  Fix: check `bytes >= sizeof(md_header)` before reading the header, then
  `bytes >= sizeof(new_order)` before each per-type cast. Same class as P2; missed
  in the first pass.

## 4. Smaller issues

- [ ] **M1 — Data-race-prone maps.** `order_map_`/`mm_order_map_` mutated from
  fill callbacks and main with no synchronization discipline; `OEClient`
  (`seq_num_`, `live_orders_`, socket) not thread-safe. Must be resolved by
  design in the multithreaded version (single-writer ownership).
- [ ] **M2 — Torn top-of-book reads.** Five separate atomics for bid/ask/qty
  can be observed in inconsistent combinations. Replace with seqlock or single
  atomic snapshot struct.
- [ ] **M3 — UB magic numbers.** `*reinterpret_cast<const uint64_t*>(str.c_str())`
  is a strict-aliasing violation and endianness-dependent. Use `memcpy` or a
  constexpr literal.
- [x] **M4 — Hardcoded IPs / exit(1) in library code.** Local interface and
  exchange addresses belong in config; the multicast helper calls `exit(1)`;
  a listener error returns silently and the bot runs on with no market data.
  *Fixed: addresses live in `config/bot.toml` (S3). `create_multicast_socket`
  moved to `src/md/multicast_socket.{h,cpp}`; it returns -1 instead of
  `exit(1)`, logs the failing step with errno, and closes the fd on every
  failure path. `run_listener` checks each socket right after creating it and,
  on failure, closes what it opened, sets `global_shutdown`, and returns.
  Tests: `test_multicast_socket`, `test_listener_startup`. Roadmap: Linux can
  silently cap `SO_RCVBUF` at `net.core.rmem_max`; read it back with
  `getsockopt` and warn (ties to P1).*
- [ ] **M5 — Misc.** `pnl.txt` written with `seekp(0)` without truncation
  (stale trailing bytes); `epoll_create(128)` deprecated → `epoll_create1(0)`;
  `MAX_MSG_SIZE` computed but unused.

---

## 5. Rework plan (in order)

1. **Fresh repo + hygiene** — strip secrets to config/env, `.gitignore`,
   CMake replacing the broken Makefile, builds clean with
   `-Wall -Wextra -Wpedantic`, CI with ASan/UBSan. *(S2, S3, M3, M4)*
2. **Fix single-threaded correctness first** — PnL accounting (C1) with unit
   tests; wire-data validation (P2); send loop (P3). Multithreading a buggy
   system multiplies the bugs.
3. **Re-architect threading** — three threads: MD listener, strategy, OE
   reader. SPSC queues between them, single-writer ownership for all mutable
   state, seqlock top-of-book. Fills processed on arrival. *(C4, M1, M2)*
4. MM rewrite in strategy/mm/ behind IStrategy, using the recovered v1 as reference; keep the requote-on-mid-change     throttle, drop MM1.
5. **Order state machine** — PendingNew/Working/PendingCancel/etc.; ACK ≠ fill
   (C2, C3); timeouts on every request (C5); IOC on arb legs (C6); distinguish
   dead connection from reject, close and stop on failure (C7).
6. **Robustness** — seq-gap detection + re-snapshot (P1); signal-handled
   shutdown with cancel-all (P4); position reconciliation loop + kill switch
   (P5).
7. **Prove it** — TSan-clean stress tests, a sim-exchange harness for the
   nasty cases (delayed ACK, fill-after-cancel, duplicate fill), README
   with before/after architecture diagrams.

Definition of done: every box checked, every fix has a test that fails on the
old behavior, and the README tells the story: *audited → fixed → re-architected.*
