# AirPlay ESP32 — Project Contract

> **This is the single source of truth for project architecture and behavioural rules.**
> Edit this file in place. Never create `PROJECT_CONTRACT_v2.md`, dated copies,
> “new” copies, or parallel contracts. Git history is the history.

**Last reviewed against project:** `v4.1.68-rt2`  
**Behavioural reference:** Shairport Sync `5.5.2`  
**Reference review date:** 2026-09-28

---

## 0. READ FIRST — NON-NEGOTIABLE RULES

1. **Shairport Sync 5.x is the behavioural reference.** If AirPlay behaviour is
   ambiguous, Shairport semantics win unless an ESP32-specific deviation is
   explicitly recorded in this file.
2. **TCP reader = transport only.** It moves framed raw bytes into the compressed
   FIFO. It does not understand RTP, SSRC, FLUSH, timing, decode or play state.
3. **Buffered Processor = the only sequential compressed-audio consumer.** No
   second cursor, FIFO scan, fast-skip path or transport-layer media discard.
4. **FLUSH is applied only by the Buffered Processor.** RTSP records requests and
   wakes the processor; the raw FIFO never applies FLUSH.
5. **Future audio is not stale audio.** AutoMix may preload roughly 90–120 s and
   then stop sending. A large positive lead alone must never delete audio.
6. **Keep future audio compressed.** The large FIFO stores preload; decode only
   near presentation time.
7. **Preserve AAC decoder history across normal discontinuities.** Timestamp
   discontinuity => decode + mute first block, not decoder reset.
8. **PTP/timeline validity is authoritative.** Immediate FLUSH invalidates the
   current buffered anchor immediately. Old timing/PCM must never leak forward.
9. **Control outranks media, but stays below network infrastructure.** RTSP
   client/control is priority 17; media decode is lower priority.
10. **Playout alone owns physical I2S output.** It consumes timestamped PCM and
    timing snapshots, not RTSP/TCP/FLUSH state.
11. **New sessions inherit no stale state.** Old FLUSH, encryption, PCM, decoder
    epoch, stream selection or timing state must not cross a true session boundary.
12. **Do not fix symptoms by adding a second state machine.** Rescue cursors,
    hidden FIFO searches and duplicate timing/FLUSH ownership are forbidden.
13. **Any accepted architectural change updates THIS SAME FILE in the same
    change.** If code and contract disagree unintentionally, treat it as a bug.

---

## 1. Project Goal

ESP32-S3 AirPlay receiver focused on:

- AirPlay 2 buffered AAC (`type 103`);
- AirPlay 2 realtime ALAC (`type 96`);
- PTP-based multiroom synchronisation;
- fast pause/resume/seek/next-track handling;
- Shairport-style immediate/deferred FLUSH;
- AutoMix / smart-playlist preload handling;
- stable long sessions on constrained embedded hardware;
- HomeKit/HAP control without allowing HomeKit to own media/timing internals.

Current accepted audio formats:

- Buffered: AAC-LC, 44.1 kHz, stereo, 1024 PCM frames/AU.
- Realtime: ALAC, 44.1 kHz, stereo, 352 frames/packet.

Known but unsupported formats are skipped safely; they are never sent to the
wrong decoder.

---

## 2. Reference Policy

### Current reference

Shairport Sync `5.5.2` defines expected protocol/state behaviour for:

- buffered TCP processing;
- AutoMix future-audio handling;
- immediate/deferred FLUSH;
- SSRC recognition;
- AAC continuity;
- PTP anchor invalidation;
- RTSP SETUP/TEARDOWN lifecycle;
- relevant malformed-input compatibility behaviour.

### When Shairport releases a new version

Before changing the reference version:

1. Read release notes.
2. Inspect relevant commits: buffered audio, RTSP, PTP/timing, session lifecycle,
   AutoMix, decoder continuity and security validation.
3. Compare the fix with our architecture.
4. Port applicable behaviour.
5. Update this file in place if any rule changes.
6. Then update the reference version above.

### Approved ESP32-specific deviations

- **6 MiB** physical buffered FIFO rather than blindly copying host memory
  sizes from Shairport. The advertised `audioBufferSize` equals the physical
  FIFO (Shairport advertises 8 MiB).
- Immediate FLUSH with `flushUntilSeq` 0 / missing ends by timestamp, not by
  sequence number (see §6).
- FreeRTOS event/semaphore waits instead of pthread/usleep mechanisms.
- ESP32 task priorities/core pinning.
- ESP32 I2S/APLL sync strategy instead of Linux backend correction methods.
- Narrower codec/rate support than desktop Shairport.

Any other behavioural difference should be treated as suspicious until reviewed.

---

## 3. Ownership Model

### Buffered path

```text
Apple TCP
   |
   v
TCP Reader                  transport only
   |
   v
6 MiB compressed FIFO       raw framed blocks
   |
   v
Buffered Processor          ONLY sequential media consumer
   |   |- FLUSH
   |   |- RTP / SSRC / timing
   |   |- decrypt
   |   `- AAC decode + discontinuity mute
   v
RTP-addressed PCM ring
   |
   v
Playout / PTP / I2S         physical output owner
```

### TCP Reader MAY

- read the buffered TCP connection;
- preserve byte order;
- assemble `[2-byte big-endian length][block bytes]`;
- write complete raw blocks to FIFO;
- apply normal backpressure;
- report connection/stream epoch;
- stop a broken transport connection;
- after an orderly close (FIN) by the sender, keep the bytes already received
  readable until the processor consumes them (Shairport `buffered_read()`
  keeps its buffer). Only stop/abort/transport errors or the next accepted
  connection discard them.

### TCP Reader MUST NOT

- parse RTP sequence/timestamp/SSRC;
- apply FLUSH;
- seek/search the FIFO;
- decide early/late/stale media;
- decode audio;
- change play/pause state;
- purge live bytes for ordinary FLUSHBUFFERED.

### Buffered Processor OWNS

- one current/held packet, read in two steps from the FIFO: the 12-byte RTP
  head first, then the body. A block that FLUSH or the SSRC gate drops at the
  head has its body consumed without being copied. This is still the single
  sequential consumer (no search, no skip-ahead, no second cursor);
- sequence/timestamp interpretation;
- SSRC recognition;
- immediate/deferred FLUSH application;
- future-packet timing gate;
- decrypt;
- AAC decoder-chain selection;
- AAC discontinuity mute policy;
- PCM publication.

No other task may independently advance buffered compressed media.

### Playout OWNS

- PCM presentation against qualified timing;
- startup/prime/alignment;
- I2S output;
- physical clock correction;
- final underrun/resync policy.

Playout MUST NOT parse TCP, RTSP, FLUSHBUFFERED or compressed FIFO state.

### Realtime ALAC

Realtime UDP receive/reorder/retransmit/staging is a separate transport path.
Do not merge buffered and realtime transport ownership merely for code reuse.
They may share common PCM/EQ/playout concepts only after valid PCM exists.

### HomeKit / HAP

HAP is a control/integration layer. It may use public APIs for volume, mute,
activity and supported accessory state. It must not directly manipulate:

- compressed FIFO cursors;
- FLUSH state;
- decoder internals;
- PTP estimator internals;
- I2S internals.

---

## 4. Conceptual State Machine

This defines behaviour; it does not require a single C enum.

```text
DISCONNECTED
    |
    v
RTSP_CONNECTED
    |
    +--> SETUP buffered --> BUFFERED_WAIT_TIMING
    |                           |
    |                 anchor + play
    |                           v
    |                    BUFFERED_PLAYING
    |                      /    |     \
    |                 pause     |      `-- future packet held
    |                    v      |
    |              BUFFERED_PAUSED
    |                           |
    |          immediate/deferred FLUSH
    |                           v
    |                 BUFFERED_FLUSH_DRAIN
    |                           |
    |                endpoint/new anchor
    |                           `--> WAIT_TIMING / PLAYING
    |
    +--> SETUP realtime --> REALTIME_WAIT_TIMING --> REALTIME_PLAYING
    |
    +--> TEARDOWN with `streams`
    |       stop stream; keep RTSP connection
    |
    `--> valid TEARDOWN plist without `streams`
            200 + Connection: close; terminate RTSP connection
```

State rules:

- Pause/play stop may clear decoded PCM/sequence-facing state but does not reset
  AAC merely because playback stopped.
- New accepted buffered TCP epoch is a true compressed-stream boundary.
- Full session boundary clears session-owned state.
- Stream TEARDOWN and full-session TEARDOWN must stay distinct.

---

## 5. AutoMix / Buffered-Audio Contract

Apple may preload about **90–120 seconds** of compressed audio and then stop
transmitting while it plays. This is normal.

Current policy:

```text
Compressed FIFO               6 MiB physical
Advertised capacity           6 MiB
Decoded target                750 ms
Decode gate                   target + 100 ms ~= 850 ms
No-valid-timing wait          ~20 ms
Very-early held-packet wait   ~2 AAC frames (~46 ms at 44.1 kHz)
```

The early wait must be interruptible by control events.

When a valid packet is too far in the future:

1. Hold at most that one packet outside the FIFO.
2. Keep it compressed.
3. Do not decode yet.
4. Do not drop merely because it is far ahead.
5. Wait for timing/control progress.
6. Re-evaluate the same packet.

The rest of the long preload stays compressed in FIFO.

Forbidden:

- future-time drop thresholds used as “stale” detection;
- FIFO RTP searching/fast skip;
- decoding the whole preload to PCM;
- purging FIFO to catch up;
- a second compressed cursor.

---

## 6. FLUSH Contract

RTSP records FLUSH state and wakes the Buffered Processor. The Buffered
Processor applies it to the one sequential packet stream.

### Immediate FLUSH

1. Disable playback immediately.
2. Invalidate buffered PTP/audio anchor immediately.
3. Wake processor.
4. Processor continues consuming compressed packets even with play disabled.
5. Drop packets before `flushUntilSeq`.
6. Endpoint packet survives.
7. Overshooting packet also survives and completes the request.
8. Immediate completion clears deferred FLUSH requests.

Immediate FLUSH must not wait for a new anchor before draining. Dropped blocks
are consumed block by block by the processor (body not copied); this is not a
FIFO fast skip.

#### Immediate FLUSH with `flushUntilSeq` 0 or missing (ESP deviation)

Shairport compares 0 modulo 2^23: when the session's sequence numbers are above
2^22 that discards every block until the sequence wraps (minutes to hours of
silence while the sender keeps streaming). Here such a request ignores sequence
numbers and ends at the first block that:

1. has RTP timestamp == `flushUntilTS` (observed on HomePod/Apple TV: that is
   the first block of the new content, the same block that ends the pending
   deferred ranges); or
2. once the sender's new anchor is valid, lies on the new timeline: RTP within
   +/-10 s of the new anchor's RTP. Measured against the anchor, never against
   the clock, so an anchor placed far in the future or a fast burst cannot
   push every new block outside the window.

The ending block survives and completion clears deferred requests, as for the
sequential rule. A later immediate FLUSH with a valid `flushUntilSeq` replaces
it. This is the only place an RTP window may decide a FLUSH endpoint.

### Deferred FLUSH

Current interval semantics:

```text
[fromSeq, untilSeq)
```

- `fromSeq` activates.
- Active-range packets are dropped.
- `untilSeq` survives and ends the range.
- Overshoot ends the range and survives.
- Active deferred FLUSH may drain while normal play is disabled.
- Maximum deferred requests: **10**.

Forbidden:

- FIFO fast skip;
- FIFO RTP/sequence search;
- stale-FLUSH rescue heuristics;
- RTP plausibility windows used to guess a FLUSH endpoint (sole exception:
  immediate FLUSH without a usable `flushUntilSeq`, above);
- raw-FIFO purge as normal FLUSHBUFFERED implementation.

---

## 7. SSRC / Codec Contract

### Unknown SSRC

```text
read block -> parse SSRC -> recognised?
                         no  -> consume/skip
                         yes -> timing/decode path
```

The first buffered block may be unknown (OS 27 behaviour fixed by Shairport
5.5.2). Decoder initialisation waits for recognised supported media.

### Recognised but unsupported

Skip safely and explicitly. Never feed it to a different decoder.

### Codec-chain rebuild

Allowed for real boundaries such as:

- recognised SSRC/format switch requiring a new chain;
- new accepted compressed TCP stream epoch;
- explicit audio-engine/session destruction;
- proven unrecoverable decoder condition.

A normal RTP gap alone is not a codec-chain boundary.

---

## 8. AAC Continuity Contract

For ordinary RTP timestamp discontinuity:

- keep AAC decoder alive;
- decode first AAC AU after the discontinuity;
- mute that block's PCM;
- preserve codec overlap/MDCT/history;
- continue normally.

Sequence-number gap alone is diagnostic and must not reset the decoder.

For ordinary AAC decode failure:

- drop the bad AU;
- do not silently reset/reopen the decoder as a generic recovery action.

Resetting the decoder because “it makes this one test work” is not acceptable
without proving a true decoder/session boundary.

---

## 9. Timing / PTP / Generation Contract

- PTP subsystem owns the clock estimate; audio reads qualified snapshots.
- The PTP estimator lifetime follows the **principal RTSP/control session**,
  not individual audio streams: clear once when a connection acquires the
  play lock, then preserve valid samples across SETPEERS -> first stream SETUP
  and across stream-level TEARDOWN/SETUP. Reset again only on a true session
  boundary, timing-mode/source incompatibility, or explicit watchdog recovery.
- Buffered audio is not released for presentation without valid qualified timing.
- Immediate FLUSH invalidates buffered anchor **when requested**, not later at
  FLUSH completion.
- Generations invalidate stale PCM in O(1); do not replace this with PCM scans.
- Old PCM must not become audible after seek, immediate FLUSH, session
  replacement or incompatible timeline change.
- GM/master changes must not reuse old mapping as though it were still valid.
- Do not bypass PTP qualification merely to shorten startup.

When changing GM/PTP behaviour, test startup and long-running multiroom playback.

---

## 10. Playout / Sync Contract

- Playout alone commits PCM to I2S.
- If code blocks/waits while control state may change, revalidate timeline/
  generation before committing output.
- Hard resync may briefly silence/re-prime instead of knowingly playing at the
  wrong time.
- Preserve learned clock correction across harmless transitions when safe.
- Decoder and RTSP tasks never write I2S directly.

The goal is stable long-term multiroom phase, not merely “audio is playing”.

---

## 11. RTSP / Task Priority Contract

Current ownership/priorities:

| Task | Core | Priority | Purpose |
|---|---:|---:|---|
| RTSP client/control (up to 3, one per connection) | 0 | **17** | FLUSH / anchor / SETUP / TEARDOWN |
| RTSP accept server | 0 | **5** | accept only |
| Event-port task (stack in PSRAM, never touches flash) | 0 | **5** | event channel: updateInfo, consume sender messages |
| Buffered AAC processor | 0 | **4** | sequential buffered consumer |
| Buffered TCP reader | 1 | **4** | raw TCP ingress |
| Audio playout | 1 | **8** | physical output |
| Realtime PCM staging | 1 | **7** | ALAC -> common PCM path |
| Realtime data RX | 0 | **7** | UDP audio ingress |
| Realtime ctrl/work/resend | 0 | **6** | recovery/processing |
| PTP | 0 | **6 legacy / 8 realtime** | clock estimator |

RTSP client priority 17 is intentionally:

- above media work so FLUSH/anchor commands are acted on quickly;
- below lwIP/ESP-IDF network/system infrastructure.

High-priority RTSP code must stay short: parse, validate, update, wake, reply.
Do not perform decode or long waits at priority 17.

Handler bodies of all RTSP connections run one at a time under the RTSP handler
mutex (`rtsp_handlers_lock()`); the play owner's disconnect cleanup runs under
it too. Lock order: play lock first, handler mutex second - never acquire the
play lock while holding the handler mutex (acquiring waits for the previous
owner's task, which may be waiting for the handler mutex).

---

## 12. RTSP / Session Lifecycle Contract

### AirPlay 2 TEARDOWN

- No valid plist: return 200; do not invent teardown semantics.
- Valid plist with `streams`: stop stream/player, keep RTSP connection.
- Valid plist without `streams`: full session teardown; return `200 OK` with
  `Connection: close`, then terminate RTSP connection.

### Dead sender detection

The RTSP control socket uses TCP keepalive like Shairport (95 s idle, 5 probes
5 s apart). A sender that disappears without FIN/RST ends the session after
~2 minutes instead of holding it (and the amplifier) forever.

### Connections and the play lock (Shairport `principal_conn`)

- Every RTSP connection has its own client task and `rtsp_conn_t`, up to
  `RTSP_MAX_CLIENTS` (3). Accepting a connection never touches audio.
- A client task gets an internal-RAM stack when possible and falls back to a
  PSRAM stack (internal RAM is short while a stream plays, which used to
  refuse a second sender). Therefore RTSP handlers and connection code must
  never touch flash: no NVS/partition/SPI-flash calls on that path. Device
  name and volume are cached by settings_init(); settings_persist_volume()
  writes from its own short-lived internal-stack task.
- The four realtime (type 96, ALAC) tasks - alac_data, alac_ctrl, alac_work,
  alac_resend - are created once, on the first realtime start, with PSRAM
  stacks, and live for the life of the device; each session wakes them with
  a task notification and they release `live_tasks` when it ends. They must
  never touch flash either. Never create ~18 KiB of internal-RAM stacks per
  SETUP again: with a second RTSP connection open that failed at ~10 KiB free.
  Objects kept for the life of the device (queues, pools) go to PSRAM
  (xQueueCreateWithCaps / heap_caps_*), never to internal RAM.
- Exactly one connection at a time holds the play lock (`conn->play_owner`).
  Only the owner may drive the global audio engine, PTP state, volume,
  amplifier and event port.
- A connection takes the play lock when it starts to play: AirPlay 2 initial
  SETUP with a timing protocol, or any stream SETUP. Taking it stops the
  previous owner's connection first; the new owner touches audio only after
  that connection's cleanup has finished.
- AirPlay 1 is not supported: a SETUP with an RTSP `Transport:` header (or a
  stream SETUP without a bplist body) is answered `461 Unsupported Transport`
  without taking the play lock, so it never interrupts the current playback.
- AirPlay 2 initial SETUP with `timingProtocol` = `None` is remote-control only:
  answered (event port 0), no play lock, playback untouched.
- On a connection without the play lock, RECORD, SET_PARAMETER, PAUSE, FLUSH,
  FLUSHBUFFERED, TEARDOWN, SETRATEANCHORTIME, SETPEERS(X) are answered `200 OK`
  without side effects (volume is remembered on that connection).
- GET /info, OPTIONS, pairing, FairPlay, GET_PARAMETER on other connections must
  never stop or reset playback.
- All slots busy: close the oldest connection that does NOT hold the play lock.
- Unknown method: `200 OK` (Shairport forces 200). Unparsable request:
  `400 Bad Request`.

### Audio start failure in SETUP

- Every failed audio start is logged with its reason and the memory state
  (no silent 500).
- A failed buffered start resets the audio engine to the hard session boundary
  (`audio_receiver_stop()`, re-init if the engine is gone) and retries once.
- After 3 consecutive failed stream SETUPs the device restarts itself: it is
  unusable in that state and previously needed a power cycle.

### Device identity (mDNS, /info, updateInfo)

The device presents itself as a HomePod mini — owner's decision (approved
deviation from Shairport Sync, which uses `ShairportSync` / `366.0` /
`0x18340405C4A00`).

- Feature bits = base `0x14340405C4A00` (Shairport Sync's AirPlay 2 bits
  without bit 47, plus bit 46 SupportsHKPairingAndAccessControl) | the bits
  selected in menuconfig "AirPlay feature bits (experimental)"
  (main/rtsp/airplay_features.h). Defaults add 47, 52, 59 and 60. Bits 64+
  are published only as the `fex` TXT key (base64 of the little-endian
  128-bit set, as HomePods do).
- `model` is `AudioAccessory5,1` (HomePod mini). One define (`AIRPLAY_MODEL`
  in rtsp_handlers.h) feeds mDNS, `/info` and `updateInfo`.
- Software version `377.40.00` (`AIRPLAY_SOURCE_VERSION` in
  rtsp/airplay_version.h): mDNS `srcvers`/`vs`, `/info` and every RTSP
  `Server: AirTunes/377.40.00` header. No other version string may exist.

### Protocol discovery trace

- `CONFIG_AIRPLAY_PROTOCOL_TRACE` is a discovery tool for sender behavior and
  feature-bit experiments. In diagnostic builds it logs every RTSP request
  occurrence, not just the first method/path pair, so state transitions remain
  observable.
- At startup the trace prints the exact effective 64-bit `features` mask, the
  extended `fex` mask and the set bit numbers.
- Every decrypted event-channel message is traceable without an arbitrary
  first-N cap.
- `/command` bodies retain their full compact bplist rendering and additionally
  log the top-level command `type`. `updateMRSupportedCommands` also opens each
  serialized bplist in `mrSupportedCommandsFromSender` and logs command ID,
  enabled state, options, byte size and content hash, so capability-set changes
  are observable even when outer message shapes look identical. If a nested
  `data` value contains a MediaRemote ProtocolMessage, the trace reports its
  numeric/name type, identifier and protobuf extension field numbers.
- `rtsp_datastream_stop()` runs inside SETUP/TEARDOWN before the reply: the
  DataStream task polls its sockets every 100 ms so stop returns quickly.
- DataStream tracing logs every message header and captures up to the first
  16 KiB of payload for bplist/MRP discovery. Large payloads are never required
  to be fully buffered merely for diagnostics.
- Protocol tracing must never change responses, audio state, timing, FLUSH,
  codec or playout behavior.

### Receiver-placed anchor (SETRATE / GETANCHOR)

- Senders from iOS 27.2 send `SETRATE {rtpTime, rate}` + polled `GETANCHOR`
  instead of SETRATEANCHORTIME. The receiver places the anchor (rtpTime
  sounds ANCHOR_LEAD_MS after placement, on the sender's PTP timeline; with a
  locked, qualified GM at once, otherwise on any valid PTP estimate after
  ANCHOR_RELAX_MS - the sender gives up after ~8 s) and returns it in the
  GETANCHOR reply with the SETRATEANCHORTIME key names. Until then GETANCHOR
  gets an empty 200. The protocol trace shows one GETANCHOR poll per SETRATE.
  Reverse-engineered; keep it traced until confirmed on real devices.

### AirPlay 2 event channel

- The event port is created by the play owner's initial SETUP. When the sender
  connects, the receiver sends one encrypted `POST /command` with the
  `updateInfo` plist (`/info` dict + `txtAirPlay`), as Shairport
  `ap2_event_send_update_info()`. The message is built in the RTSP task before
  the event task starts: the event task's stack is in PSRAM and must not read
  NVS/flash.
- Keys: HKDF-SHA512 "Events-Salt"; the receiver encrypts with
  "Events-Write-Encryption-Key" and decrypts with "Events-Read-Encryption-Key"
  (Shairport server cipher channel 4), each with its own nonce counter starting
  at 0 per event connection. Framing is the control-channel framing.
- Everything the sender sends on the event channel is read and consumed
  (decrypted and logged when possible, otherwise discarded). Never leave
  received bytes in a socket that is polled with select().

### Session replacement

A new true session must not inherit:

- immediate/deferred FLUSH requests;
- stale encryption key state;
- old stream selection;
- stale PCM generation;
- decoder stream epoch;
- invalid old anchor/timing;
- old connection-specific control state.

---

## 13. AirPlay 2 Input Validation Contract

### `shk`

Accept only **exactly 32 bytes** for ChaCha20-Poly1305-IETF.

- no truncation of long keys;
- no zero-padding of short keys;
- parser must preserve real source length for validation.

### `timingPeerInfo.Addresses` / SETPEERS list

If an array element (SETPEERSX `Addresses`, or a plain SETPEERS entry) is not a
string:

- skip that element;
- keep valid string addresses;
- do not crash;
- do not reject the whole list because one element is malformed.

### General remote-input rule

RTSP / bplist / TLV parsing must have bounds and forward-progress checks.
In the bplist parser every wire count/length is checked with the overflow-free
`bplist_span_ok()` / `bplist_read_ext_len()` helpers (size_t is 32 bits on the
ESP32), and the recursive key search has a global visit budget.
Malformed network input must not cause OOB access, infinite loops, unbounded
stack use or partially-valid crypto state.

---

## 14. Wi-Fi / TCP Contract

Current performance configuration:

```ini
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=24
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=85
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=32
CONFIG_ESP_WIFI_RX_BA_WIN=32
CONFIG_LWIP_TCP_WND_DEFAULT=65535
CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
```

Wi-Fi power saving is disabled during normal AirPlay operation:

```c
esp_wifi_set_ps(WIFI_PS_NONE);
```

Principle:

```text
Wi-Fi/lwIP buffers -> short bursts/jitter
6 MiB compressed FIFO -> long AutoMix preload
```

Do not turn lwIP into the 90–120 s media store.

When `sdkconfig.defaults` changes, remember an existing generated `sdkconfig`
may retain old values. Verify the actual build in:

```text
sdkconfig
build/config/sdkconfig.h
```

---

## 15. Memory / Shared Workspace Contract

- Buffered FIFO is currently 6 MiB and the advertised `audioBufferSize` is the
  same: a full FIFO stops the TCP reader and TCP backpressure holds the
  sender; nothing is dropped.
- Buffered and realtime modes may share large codec/media workspace only with
  exclusive ownership.
- Realtime start must not reuse shared workspace while buffered transport/
  processor still owns it.
- Hard session-boundary cleanup may clear FIFO; live FLUSHBUFFERED may not use a
  FIFO clear as its normal behaviour.
- Before enlarging buffers, account for FIFO, PCM, decoder, realtime pools,
  task stacks and Wi-Fi/lwIP memory together.

A larger buffer is not an optimisation if it reduces long-session stability.

---

## 16. Concurrency Rules

- Every mutable subsystem has one logical owner.
- Cross-core compound state uses real synchronization, not timing luck.
- Never hold control mutexes across codec decode, socket blocking I/O, long
  waits or I2S operations.
- Event-driven wakeups are preferred to tight polling.
- Arbitrary sleeps are not a correctness mechanism for races.
- After a blocking wait, revalidate generation/timeline before committing media
  if control state could have changed.

---

## 17. Forbidden Regressions

Do not reintroduce any of these without explicitly redesigning this contract:

- FLUSH logic in TCP reader;
- multiple compressed FIFO consumers;
- FIFO sequence/RTP search or rewind;
- live FIFO purge for ordinary seek/FLUSH;
- future AutoMix audio dropped solely for being far ahead;
- AAC reset on every RTP gap;
- decoder/RTSP direct I2S writes;
- playout parsing RTSP/compressed media;
- old anchor reused after immediate FLUSH;
- old FLUSH requests crossing into a new buffered session;
- full TEARDOWN connection kept alive as stream-only teardown;
- a new RTSP connection (probe, pairing, remote control) stopping playback
  before it takes the play lock;
- polling a socket with select() and only MSG_PEEKing it (unconsumed data
  makes select() fire forever: a busy loop that starves lower-priority tasks);
- malformed `shk` accepted via truncation/zero-padding;
- a new workaround left beside the obsolete workaround it replaced.

---

## 18. Change Discipline — UPDATE THIS SAME FILE

### Before changing code

1. Read this file.
2. Identify the owner/layer that should implement the change.
3. Identify which invariant it touches.
4. For AirPlay semantics, compare with current Shairport before inventing a new
   behaviour.
5. Prefer modifying the existing owner/state machine over adding a parallel path.

### This file MUST be updated in the same change when modifying

- layer ownership;
- FIFO/cursor behaviour;
- FLUSH semantics;
- AutoMix handling;
- major buffer/decode-lead policy;
- AAC continuity/reset policy;
- supported formats;
- PTP/GM/timeline semantics;
- stale-PCM/generation policy;
- RTSP/session lifecycle;
- task priorities/core ownership;
- Wi-Fi/network buffer strategy;
- shared-memory ownership;
- HAP/HomeKit ownership;
- an intentional deviation from Shairport.

Pure refactors/renames/comments do not require a contract change if behaviour and
ownership truly remain identical.

### How to edit this file

This file describes the **current accepted system**, not historical versions.

When a rule changes:

- edit/replace the old rule in place;
- delete text that is no longer true;
- add the new rule in the correct section;
- update `Last reviewed against project` at the top;
- never create another contract file;
- use Git history for previous rules.

If code conflicts with this contract, only two outcomes are valid:

1. the code is wrong and must be corrected; or
2. the architecture is intentionally changing and this contract is revised in
   the same change.

There is no silent third option.

---

## 19. Regression Checklist for Architectural Changes

### Buffered / AutoMix

- cold-start AAC;
- 90–120 s AutoMix burst preload;
- sender silence while FIFO backlog keeps playing;
- pause/resume with future audio buffered;
- seek with a large backlog;
- next/previous with high FIFO occupancy;
- repeated immediate FLUSH;
- deferred FLUSH activation/end/overshoot;
- unknown first SSRC;
- unknown SSRC between valid blocks;
- AAC decode error followed by recovery without unnecessary reset.

### Session lifecycle

- stream TEARDOWN with `streams`, then new SETUP on same RTSP connection;
- full TEARDOWN without `streams`, confirming connection closes;
- rapid old-player -> new-player replacement;
- /info probes, pairing and a second device's connection during playback
  (playback must continue; log "acquired / takes the play lock" only on play);
- no old PCM audible after fast stop/start;
- new buffered session has no old FLUSH requests.

### Timing / multiroom

- stable PTP startup;
- pause/resume with same GM;
- add/remove HomePod or another peer during playback;
- GM handover/reselection;
- long buffered playback;
- realtime ALAC for 30+ minutes for drift checks.

### Control / networking

- FLUSH/anchor while AAC decode is busy;
- large Wi-Fi burst into FIFO;
- high FIFO occupancy/backpressure;
- RTSP remains responsive under media load.

### Validation

- exact 32-byte `shk` accepted;
- short/long `shk` rejected safely;
- mixed valid/non-string timing peer list retains valid addresses.

---

## 20. New Chat / New Developer Entry Point

When opening this project without conversation history:

1. Read `PROJECT_CONTRACT.md` first.
2. Read only the files owned by the affected layer initially.
3. For buffered audio, trace:

```text
rtsp_handlers
  -> buffered control update
  -> ap2_buffered_fifo        (transport only)
  -> ap2_buffered_processor_task
  -> decrypt / AAC decoder
  -> pcm_rtp_ring
  -> ap2_playout_task
  -> I2S
```

4. For realtime audio, trace the separate realtime receiver/stage path.
5. Check Shairport only for the protocol behaviour relevant to the requested
   change.
6. Before deleting code, verify whether it protects a rule in this contract.
7. Before adding a workaround, prove the existing owner cannot express the
   required behaviour cleanly.
8. If accepted behaviour changes, update this same file in the same patch.

Suggested instruction for a new chat:

> **Read `PROJECT_CONTRACT.md` first and treat it as the project architecture
> contract. Do not make a change that contradicts it unless the requested design
> intentionally changes the contract; in that case update the same file in the
> same patch.**

---

## 21. Definition of Done

For architectural work, “done” means:

- change is implemented in the correct ownership layer;
- no obsolete parallel state/workaround remains active;
- Shairport semantics were checked where relevant;
- stale session/timeline/media state cannot leak forward;
- AutoMix assumptions remain valid;
- control responsiveness remains valid;
- long-term sync is not traded for a one-test startup hack;
- relevant regression scenarios were considered/tested;
- this same file was updated if accepted rules changed.

**The goal is predictable, explainable long-session behaviour — not a collection
of local fixes that happen to pass one test.**
