# AirPlay ESP32 — Project Contract

> **Single source of truth for the `new-airplay-control` architecture.**
> Update this file in place when accepted architecture changes. Git history is
> the history; do not create parallel contract files.

**Last reviewed against project:** `v4.1.80-apap-frame-queue`  
**Primary type-103 reference:** observed iOS 27.2 / HomePod-style APAP wire
behaviour plus Apple framework/decompile evidence  
**Secondary reference:** Shairport Sync for codec-continuity and general AirPlay
playout behaviour where it is still applicable

---

## 0. Non-negotiable rules

1. **Buffered type 103 is APAP-only on this branch.** There is no legacy
   buffered RTP-over-TCP byte FIFO runtime.
2. A type-103 SETUP is accepted only when it negotiates:
   `streamConnectionTypeAPAP + streamConnectionTypeMediaDataControl +
   streamConnectionKeyUseStreamEncryptionKey` with a 32-byte `shk`.
3. **APAP RX owns transport only:** BE32 package framing, APAP header parsing,
   ChaCha20-Poly1305 authentication/decryption, extension parsing and semantic
   AAC-frame enqueue.
4. **The APAP frame queue stores meaning, not anonymous bytes.** Every entry
   carries `seq + 64-bit media_sample + original mediaTime + AAC payload`.
5. **AAC decode is a separate consumer.** Network reception never performs AAC
   decoding and never writes I2S directly.
6. **Decoded APAP PCM enters the common audio path at one boundary only:**
   `PCM -> EQ -> timed PCM ring -> PTP scheduler -> I2S servo -> DMA`.
7. **Playout alone owns physical I2S.** RTSP, MDC, APAP RX and decoder tasks may
   never enable/disable/write I2S directly.
8. **APAP media time stays 64-bit until final PCM publication.** Only the common
   PCM ring uses the low 32-bit sample address because it is shared with RTP
   realtime audio and has a finite ~3 s window.
9. **`strt` + `anch` define the presentation map.** APAP media time is not PTP;
   the anchor maps one media sample to one PTP instant.
10. **`fshb` is applied against semantic APAP boundaries.** Sequence is
    authoritative when supplied; mediaTime is an independent fallback/check.
11. **Preserve AAC decoder history across ordinary track/seek discontinuities.**
    Do not recreate the decoder for `fshb`; reset EQ history and mute the first
    post-boundary PCM AU.
12. **Realtime ALAC (type 96) is a separate RTP/UDP path.** It may share EQ,
    timed PCM and playout after valid PCM exists, but not APAP transport state.
13. **Stream-specific TEARDOWN stays stream-specific.** Tearing down Remote
    Control type 130 must not tear down active type-103 APAP audio.
14. **No rescue state machines.** Do not add hidden byte scans, second compressed
    cursors, speculative stale-data deletion, or alternate I2S ownership.

---

## 1. Supported audio paths

### Buffered APAP AAC — type 103

Supported format on this branch:

- AAC-LC
- 44.1 kHz
- stereo
- 16-bit decoded PCM
- 1024 PCM frames per AAC access unit
- APAP stream encryption using the negotiated 32-byte `shk`
- MediaDataControl for `magc`, `strt`, `anch`, `fshb`, `amsm`

Runtime ownership:

```text
iPhone / Apple sender
        |
        v
BufferedAPAP TCP
        |
        |  BE32 package length
        |  15-byte APAP header
        |  mediaTime + 24-bit seq
        v
APAP RX task
        |
        |  ChaCha20-Poly1305-IETF
        |  AAD = APAP header bytes 0..11
        |  body = ciphertext || tag16 || explicit nonce8
        |  parse APAP extension chain
        v
Semantic APAP frame queue (PSRAM)
        |
        |  {seq, media_sample64, mediaTime, AAC AU}
        v
AAC decoder task
        |
        v
Common EQ
        |
        v
Timed PCM ring
        |
        v
media_sample <-> PTP scheduler
        |
        v
I2S ppm servo / DMA
```

### Realtime ALAC — type 96

Realtime ALAC remains independent:

```text
RTP/UDP + retransmit
        |
        v
ALAC decrypt/decode/reorder
        |
        v
ordered PCM staging
        |
        v
Common EQ
        |
        v
Timed PCM ring
        |
        v
PTP scheduler / I2S
```

Do not reintroduce the deleted type-103 RTP/TCP FIFO merely to share code with
realtime RTP.

---

## 2. APAP semantic frame queue

Current queue contract:

```text
64 frame slots
max AAC payload per slot       4096 bytes
payload pool                   ~256 KiB
nominal frame duration         1024 / 44100 = 23.22 ms
nominal queue time capacity    ~1.49 s
```

Each queue entry contains:

```c
seq             // APAP 24-bit packet sequence
media_sample    // normalized 64-bit 44.1-kHz sample address
media_time      // original APAP mediaTimeValue
media_timescale // original APAP mediaTimeScale
aac_len
aac[]
```

Rules:

- Queue order is TCP/APAP order.
- There is exactly one producer (APAP RX) and one consumer (AAC decoder).
- `fshb` may clear the queue from the MDC/control task; queue operations use a
  task mutex, never a long interrupt-disabled memcpy critical section.
- When the queue is full, APAP RX stops consuming TCP. TCP flow control is the
  upstream backpressure mechanism.
- Do not add a second 6 MiB anonymous compressed cache behind or in front of it.
- Metadata-only authenticated APAP boundary packets (`aac_len == 0`) are valid
  queue entries because their `seq/mediaTime` may be the exact `fshb` endpoint.

---

## 3. Media time model

APAP packets carry media time, not absolute network presentation time.

Normalize once using exact integer arithmetic:

```text
media_sample = floor(mediaTimeValue * sampleRate / mediaTimeScale)
```

The implementation must not require `__int128` and must not use floating point
for the timeline conversion.

Observed equivalence that is kept as a regression test:

```text
28019334008224 / 1000000000 @ 44100 -> 1235652629
70758001719561 / 1000000000 @ 44100 -> 3120427875
```

For normal APAP audio headers the timescale is 44100, so `mediaTimeValue` is
already the sample-domain address.

The common PCM ring remains 32-bit addressed because it is finite (~2.97 s) and
shared with RTP. Conversion to low 32 bits happens only at publication into that
ring. Sequence/timeline decisions before that point use the full 64-bit APAP
sample address.

---

## 4. `strt` / `anch` timing contract

`strt` establishes a new media epoch. `anch` requests the receiver-selected
presentation anchor for that epoch.

The receiver chooses one stable mapping per `strt` epoch:

```text
anchor_media_sample <-> anchor_ptp_ns
```

Every packet is then scheduled by:

```text
presentation_ptp = anchor_ptp
                 + (packet_media_sample - anchor_media_sample) / sample_rate
```

Repeated `anch` requests for the same `strt` epoch must return the same anchor;
do not move the presentation timeline merely because the sender asks again.

A new accepted `strt` creates a new mapping. Old PCM/timing must not leak into
that epoch.

---

## 5. PCM buffering and decode lead

The proven common timed PCM ring is retained unchanged in principle:

```text
128 pages x 1024 stereo frames
= 131072 frames
= ~2.97 s @ 44.1 kHz
```

APAP decode deliberately stays much closer to the playhead:

```text
decoded target             ~750 ms
APAP decode lead ceiling   ~850 ms
startup reserve            ~250 ms
```

If decoded APAP would exceed the lead ceiling, the decoder waits. This naturally
fills the semantic APAP queue and finally applies TCP backpressure.

Do not decode a long future preload to PCM merely because it is available.

---

## 6. `fshb` / transition contract

MediaDataControl `fshb` may contain both sequence and media-time boundaries:

```text
flushFromSeq
flushUntilSeq
flushFromMediaTimeValue / Scale
flushUntilMediaTimeValue / Scale
```

### Sequence comparison

APAP sequence is 24-bit modular. Use signed 24-bit delta semantics; never compare
with plain unsigned `<` across wrap.

### Immediate flush (`until` only)

1. Clear queued compressed APAP frames immediately.
2. Invalidate the current timed PCM generation immediately.
3. Invalidate current presentation timing.
4. Keep receiving/decrypting APAP.
5. Drop semantic frames until the exact `until` boundary is reached.
6. The boundary frame itself may be metadata-only and is still valid.
7. Wait for/use the new `strt` + `anch` map before publishing audible PCM.

### Deferred flush (`from` + `until`)

1. Continue normal playback until `from` is reached.
2. At `from`, invalidate old PCM/timing and reset EQ history.
3. Drop frames in the transition interval.
4. At `until`, stop dropping.
5. Resume publication only under the correct new timeline/anchor.

Sequence is authoritative when present. Media sample is used when sequence is
absent and remains useful as an independent sanity signal.

A later immediate `fshb` supersedes an earlier deferred transition state.

---

## 7. AAC continuity

The AAC decoder is created once for the APAP session:

```text
AAC-LC / 44100 / stereo / 16-bit PCM
```

Do not destroy/recreate it on ordinary `fshb`, seek, AutoMix transition or track
change. A malformed AU is dropped while the decoder chain stays alive.

At an accepted media discontinuity:

- reset EQ filter history;
- preserve decoder overlap/history;
- decode the first new AU but mute its PCM output;
- subsequent valid AU output is normal.

A full session teardown destroys the decoder.

---

## 8. Memory ownership

### APAP

Owned by the APAP transport context:

- ~256 KiB semantic AAC frame queue in PSRAM;
- one APAP package decrypt buffer;
- one AAC decoder input scratch buffer;
- one decoded PCM scratch block;
- APAP RX and decoder task stacks in PSRAM.

### Common audio engine

Owned by `audio_receiver`:

- final timed PCM ring;
- playout state;
- EQ state;
- PTP/media mapping;
- I2S completion/sync/servo state.

### Realtime ALAC

The former shared multi-megabyte codec/FIFO workspace is gone. The retained
workspace is sized only for:

- ALAC raw PCM staging ring;
- realtime DATA/RTX packet pools.

APAP does not borrow or compete for that workspace.

---

## 9. Control and lifecycle

### Type-103 SETUP

Required:

```text
streamConnectionTypeAPAP
  streamConnectionKeyUseStreamEncryptionKey = true
streamConnectionTypeMediaDataControl
32-byte shk
```

Legacy type-103 RTP/TCP is intentionally unsupported on this branch and receives
`461 Unsupported Transport`.

If a request also advertises RTP/RTCP alternatives, they are not mirrored in the
buffered response. This rule does not affect type-96 realtime RTP.

### Type-130 Remote Control

Remote Control has its own DataStream lifetime. Type-130 TEARDOWN closes only
that stream and must preserve active type-103 APAP/MDC/audio.

### Full TEARDOWN

A full valid session teardown stops APAP/MDC, clears stream encryption and media
state, and allows the common playout engine to quiesce before a new session.

---

## 10. What was intentionally removed

The following are not part of this branch architecture and must not be
reintroduced without an explicit design review:

- `ap2_buffered_fifo.c/.h`;
- 6 MiB type-103 raw TCP byte FIFO;
- legacy buffered RTP/SSRC packet processor;
- buffered-RTP sequence/timestamp scan/skip logic;
- old FIFO RX diagnostics and FIFO host tests;
- smoke-mode direct I2S output;
- fixed smoke attenuation;
- APAP observer-only transport;
- multiple speculative APAP cryptor probes;
- plaintext APAP fallback;
- dynamic per-packet AAC decoder recreation.

The RTP crypto/parser code that remains exists for realtime ALAC type 96, not
for buffered AAC.

---

## 11. Diagnostics

Normal APAP status should expose semantic and presentation state, e.g.:

```text
AAC/APAP | sync=... | i2s=... | ptpD=... | gm=...
         | q=frames/capacity bytes/capacity | pcm=...ms
```

Do not restore raw packet hex dumps to normal operation. Protocol trace can be
used deliberately when investigating a new OS/protocol change.

Useful APAP lifecycle logs:

- negotiated APAP/MDC + 32-byte key;
- first authenticated APAP package;
- queue occupancy/high water;
- `strt` / stable `anch` mapping;
- deferred/immediate `fshb` entry and completion;
- periodic decoded/published frame counts;
- full teardown/disconnect reason.

---

## 12. Regression expectations

Before accepting changes to this path, at minimum verify:

- exact media-time conversion regression;
- semantic APAP queue push/pop/full/clear;
- APAP+MDC SETUP response contains APAP and MDC, not buffered RTP/RTCP;
- type-130 TEARDOWN does not stop audio;
- `strt`/`anch` response plist remains parseable;
- track change / seek does not recreate the AAC decoder;
- immediate/deferred `fshb` reaches the declared APAP boundary;
- EQ remains in the decoded PCM path;
- only playout writes I2S;
- realtime ALAC still starts, stages and plays independently.

The host-test suite is necessary but not sufficient. Final validation is an
ESP32-S3 IDF build plus real iOS/HomePod-style playback including next-track,
seek, pause/resume and long-running sync.
