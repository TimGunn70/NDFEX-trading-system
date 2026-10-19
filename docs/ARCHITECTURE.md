# Architecture

> **Skeleton.** Headings and prompts only — the substance is yours to write.
> Delete each prompt as you answer it. If you cannot answer one, that is the
> part of the design you do not yet understand, which is exactly what an
> interviewer will find.

## 1. What the system does

<!-- Two or three sentences. What arrives, what leaves, what decision sits in
     between. Assume the reader has never heard of NDFEX or ETF create/redeem. -->

## 2. Current design (as inherited)

<!-- Prompt: draw the real thing, not the intended thing. One process, how many
     threads actually exist, and which of them touches which state.

     Facts to anchor it:
       - main.cpp spawns an MD listener thread and a PnL printer thread.
       - No strategy thread exists at all (AUDIT.md S1).
       - Fills are only drained while a caller is blocked inside
         OEClient::wait_for_response() (C4).

     Then answer: where does market data enter, where do orders leave, and at
     what point does a fill become a position change? -->

### 2.1 Why it fails

<!-- Prompt: pick the two or three findings that are consequences of the
     *structure*, not of a typo. C4 is the obvious one. Explain the causal
     chain: no reader thread -> fills queue in the socket buffer -> the
     "sell until flat" loop polls a position that cannot change -> oversell.

     This section is the one you will actually be asked about. -->

## 3. Target design

<!-- Prompt: three threads — MD listener, strategy, OE reader. For each one,
     state: what it owns exclusively, what it reads, and what it is forbidden
     from touching. If two threads can write the same bytes, say why that is
     safe or change the design. -->

### 3.1 Thread ownership table

| Thread | Owns (sole writer) | Reads | Never touches |
|---|---|---|---|
| MD listener | | | |
| Strategy | | | |
| OE reader | | | |

<!-- Prompt: fill this in before writing any threading code. If a cell is hard
     to fill, the design is not finished. -->

### 3.2 Queues between threads

<!-- Prompt: for each edge in the diagram — what type, what capacity, and what
     happens when it is full. "It won't fill up" is not an answer; say what the
     code does when it does.

     Be able to defend: why SPSC rather than a mutex + condition_variable? What
     is the actual measured or argued cost you are avoiding? An interviewer who
     hears "lock-free is faster" and nothing else will push, so have the real
     reason ready — or admit a mutex would be fine here and say why you still
     chose otherwise. -->

### 3.3 Top-of-book publication

<!-- Prompt: M2 says five independent atomics can be read in inconsistent
     combinations. Describe the replacement.

     If you choose a seqlock: write the exact store/load ordering and say which
     memory_order each operation needs and why. If you choose a single atomic
     snapshot struct: say whether it is lock-free on arm64 at that size, and how
     you checked rather than assumed. -->

## 4. Order lifecycle

<!-- Prompt: the state machine. States, the events that move between them, and
     the transitions that must be rejected.

     The bug this replaces: ACK was treated as a fill (C2), so a resting order
     counted as hedged. State the invariant that makes that impossible to write
     again, not just the corrected code. -->

```
<!-- states and transitions -->
```

## 5. Failure handling

<!-- Prompt: one line each for what the system does when —
       - a market-data sequence number gaps (P1)
       - the OE socket dies mid-write (P3)
       - a request gets no response at all (C5)
       - SIGINT arrives with live orders resting (P4)
       - local position disagrees with the exchange (P5) -->

## 6. What I would do differently with more time

<!-- Prompt: name the shortcuts honestly. This section is worth more in an
     interview than the rest of the document, because it is the one they cannot
     predict. Include at least one thing you got wrong and had to back out. -->
