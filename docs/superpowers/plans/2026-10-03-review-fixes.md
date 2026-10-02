# Review fixes implementation plan

> **For agentic workers:** Use superpowers:executing-plans and dispatching-parallel-agents for the independent file groups below. User has authorized implementation; no tests, build or flash.

**Goal:** Correct the demonstrated error paths and concurrency hazards, and remove redundant work without changing working audio timing.

**Architecture:** Preserve single owners, generation/FLUSH checks, buffer capacities and decoder history. Apply bounded parsing and lifecycle guards before CPU/memory optimizations. Deferred profiling candidates remain explicit, not claimed fixed.

**Tech Stack:** ESP-IDF 5.5.5, ESP32-S3, FreeRTOS, C.

**Spec:** docs/optimization-review-2026-10-03.md

## Global constraints

- No tests, build, flash or commits, as instructed by the user.
- Preserve all existing changes and file line endings.
- PCM final/raw capacities, Wi-Fi TX configuration, task stack capacities and timing margins remain unchanged.
- Static verification consists of source review, caller/interface searches and whitespace/diff checks.

## Review focus

- Malformed object counts, integer overflow and cyclic plist references must terminate within bounded work.
- Restart must never reuse a slot or object while its prior owner is alive.
- Partial encrypted frames and authentication failure must terminate without retrying broken framing.
- Repeated SETUP must not mutate the format/key of a live producer.
- Queue floods and lost recovery events must be visible without hot-path log storms.

## Independent work groups

### 1. Plist parser
- [x] Harden count/length arithmetic and extended integer widths in main/plist/bplist_parser.c.
- [x] Bound recursive traversal by a shared request visit budget and cycle path checks.
- [x] Review all object/ref pointer calculations without executing tests.

### 2. RTSP
- [x] Fix fatal crypto read contract, bounded send and strict restart guards in rtsp_server/crypto/message.
- [x] Synchronize event socket lifecycle; consume unsupported inbound event data by closing the event connection with a rate-limited diagnostic.
- [x] Validate SETUP locally and reject incompatible live reconfiguration; fix modular progress.
- [x] Reduce repeated header parsing/allocation where the same framing validation permits it.
- [x] Review start/stop/cancel and API callers without changing application protocols speculatively.

### 3. Logs and PTP
- [x] Replace log ring byte loops with wrap memcpy, add broadcaster lifecycle guard and safe detach API.
- [x] Guard PTP init after incomplete stop and move snapshot derivative work outside locks where coherent.
- [x] Preserve snapshot freshness and bounded socket sends.

### 4. Audio and integration
- [x] Remove idle main task loop and unused FIFO not_empty semaphore.
- [x] Avoid payload startup memset; bound resend event batches and report overflow with counters.
- [x] Apply neutral EQ fast path preserving dither silence state, word-based contiguous checks and I2S init cleanup.
- [x] Fix calibration zero-tick error delay and decoder log storms.
- [x] Review scratch ownership before sharing; do not claim unverified library minimum capacities.
- [x] Integrate detach/caller changes, source-review all edits, and record implemented/deferred findings.

## Completion record

Implementation and source review completed. Fresh reviewer found the delayed static DATA reaping issue; cleanup now retries stop until true idle. Source whitespace checks pass under main/. No tests, build, flash or commits. Device behavior and measured performance remain unverified.

Ruling: use the current working checkout because it contains the user's working firmware changes; preserve those changes rather than starting from another Git revision.
Ruling: scope the implementation to demonstrated defects and grounded optimizations. Geometric RTSP growth, parser-context performance redesign, recovery snapshot caching/bitmap, and cold control/NVS scratch changes remain profiling candidates, detailed in docs/review-fixes-2026-10-03.md.
