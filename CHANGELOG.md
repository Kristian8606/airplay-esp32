# v4.1.93-ptp-timingpeerinfo

- Restores v4.1.91 keep-open behavior after the type-130 teardown; no Connection: close probe.
- PTP no-stream SETUP response now includes `timingPeerInfo` with the ESP receiver IPv4 address in both `Addresses` and `ID`, matching documented AirPlay 2 PTP receiver responses.
- RemoteControl-only initial SETUP (`timingProtocol=None`) is unchanged and still returns only eventPort/timingPort.
- Keeps RSESSION lifecycle diagnostics for correlation.

# v4.1.91-session-lifecycle-diag

Diagnostic-only build based on v4.1.90. No AirPlay transport/audio behavior is intentionally changed.

- Adds stable `RSESSION` connection IDs for each accepted RTSP TCP socket.
- Logs play-lock acquisition and final socket cleanup with the same connection ID.
- Logs SETUP/RECORD/TEARDOWN/SETPEERS/SETRATEANCHORTIME correlation: fd, CSeq, path, owner/encryption/audio state, `Session`, `X-Apple-Session-ID`, `Connection`, `User-Agent`, `DACP-ID`, and `Active-Remote`.
- Summarizes relevant bplist identity/lifecycle fields: `sessionUUID`, `sessionCorrelationUUID`, `groupUUID`, `timingProtocol`, `isRemoteControlOnly`, `channelID`, `clientUUID`, `clientTypeUUID`, `clientID`, `controlType`, `seed`, `wantsDedicatedSocket`, `streamConnectionID`, and stream type.
- Explicitly logs whether a type-130 TEARDOWN keeps the RTSP control connection open or a full TEARDOWN replies with `Connection: close`.
- Heavy `AIRPLAY_PROTOCOL_TRACE` remains optional/off, so the diagnostic should not materially perturb timing.

# v4.1.90-homepod-updateinfo

- Expanded event-channel `updateInfo` to mirror the HomePod mini / `AudioAccessory5,1` receiver-state shape.
- Added `playbackCapabilities`, `canRecordScreenStream`, `keepAliveSendStatsAsBody`, `volumeControlType`, `senderAddress`, `screenDemoMode`, `initialVolume`, `featuresEx`, `supportedFormats`, `macAddress`, and `receiverHDRCapability`.
- Preserves this ESP receiver's own name, MAC/device ID, pi/psi, Ed25519 public key, current feature mask/fex, source version, status flags, and TXT record.
- Logs the decoded `updateInfo` plist before encryption for direct comparison during the RCS test.
- No changes to APAP, MDC, PTP, AAC, RemoteControl DataStream framing, or feedback behavior.

## v4.1.89-rc-feedback-keepalive

- RemoteControl-only `/feedback` now returns the AirPlay 2 keepalive plist `{streams: []}` instead of an empty 200 response.
- Added dedicated diagnostics: `RC-KEEPALIVE ... streams=[]`.
- No APAP, MDC, PTP, audio scheduling, feature-bit, or MediaRemote state changes.

# v4.1.88-menuconfig-feature-bits

- Reworked AirPlay feature advertisement for A/B testing.
- Removed the all-or-nothing HomePod-mini feature-mask switch.
- Every bit currently present in the v4.1.87 advertisement is now an independent
  `menuconfig` checkbox, including the previously hard-coded base bits 9, 11, 14,
  18, 19, 20, 22, 30, 38, 40, 41, 46 and 48.
- Bit 58 `SupportsHangdogRemoteControl` is visible and defaults OFF, matching v4.1.87.
- Bit 38 `SupportsUnifiedMediaControl` is separately visible for the next A/B test.
- Extended HomePod `fex` bits are also independently selectable.
- Defaults reproduce v4.1.87 exactly: `features=0x4A7FCA00,0x38356BD0` and
  `fex=AMp/StBrNTwQoa7YDQ`.
- No RTSP, RCS, APAP, MDC, PTP or audio pipeline behaviour changed.

# v4.1.87-no-hangdog-rc-test (based on v4.1.86)

Diagnostic A/B only: clear AirPlay feature bit 58 (`SupportsHangdogRemoteControl`)
from the otherwise exact HomePod mini 27.2 advertisement. `features` becomes
`0x4A7FCA00,0x38356BD0`; `fex`, model, APAP, PTP, type-103 stream connection
and MediaDataControl support are unchanged. This tests whether bit 58 is what
causes Apple Music to create the short-lived remote-control-only/type-130 RCS.

# v4.1.81-feedback-diag (based on v4.1.80-apap-frame-queue)

Passive diagnostics only; APAP, DSP and timing behavior are unchanged.

- Logs every RTSP `/feedback` request and exact bplist response with per-session
  RX/TX counters and milliseconds since the type-103 audio SETUP.
- Logs every encrypted DataStream `sync/* -> rply` transaction, including
  empty replies, send success/failure, reply payload size and elapsed time.
- Adds a control summary at TEARDOWN so a ~60 s sender shutdown can be
  correlated directly with the last feedback/MDC activity.
- Adds internal-RAM and PSRAM free/largest-block telemetry to the existing
  2-second AAC/APAP status line to rule memory leaks in or out without changing
  the audio path.

# v4.1.80-apap-frame-queue (based on v4.1.79-apap-pipeline-buildfix)

- Make modern Buffered APAP the only type-103 audio transport on this branch.
  SETUP now requires APAP + MediaDataControl + stream encryption with a 32-byte
  `shk`; legacy buffered RTP/TCP is rejected instead of reviving the old FIFO.
- Delete `ap2_buffered_fifo.c/.h` and the legacy buffered-RTP processor path,
  including FIFO RX/flush diagnostics and their host tests.
- Add a semantic PSRAM APAP queue: 64 frames x 4096-byte maximum AAC AU (~256
  KiB, ~1.49 s at 1024 frames/AU). Each entry carries the 24-bit APAP sequence,
  full 64-bit normalized media sample, original mediaTime and AAC payload.
- Split APAP into a network producer (framing/decrypt/extensions/enqueue) and a
  decoder consumer. Queue-full backpressure stops TCP reads naturally. Queue
  copies are protected by a task mutex rather than a long interrupt-disabled
  critical section.
- Keep the proven post-decode path unchanged: AAC PCM -> common EQ -> timed PCM
  ring -> PTP scheduler -> tagged I2S -> ppm servo. APAP never writes I2S.
- Keep APAP media addresses 64-bit until final PCM-ring publication; factor the
  exact quotient/remainder `mediaTime -> sample` conversion into one shared
  helper and add trace-derived host regressions.
- Apply `fshb` in the semantic consumer using both APAP sequence and mediaTime.
  Immediate flush clears queued compressed frames and PCM immediately; deferred
  flush keeps normal playback until `from`, then drops to `until`.
- Create the AAC decoder once per APAP session and preserve it across ordinary
  track/seek/fshb boundaries. EQ history is reset and the first post-boundary
  PCM AU is muted.
- Remove smoke/observer scaffolding: direct I2S, fixed -12 dB output, raw APAP
  hex dumps, alternate cryptor probes, plaintext fallback and dynamic decoder
  recreation. The retained encrypted format is the empirically working iOS
  27.2 `ChaCha20-Poly1305-IETF + explicit nonce8 + APAP12 AAD` path.
- Shrink the old shared codec workspace to realtime-ALAC staging + DATA/RTX
  packet pools only; APAP owns its compact queue independently.
- Keep realtime type-96 ALAC/RTP intact and separate from APAP transport state.
- Update `PROJECT_CONTRACT.md` to make the APAP semantic-frame architecture the
  source of truth for this branch.

# v4.1.79-apap-pipeline-buildfix (based on v4.1.79-apap-pipeline)

- ESP32-S3/Xtensa build fix: remove unsupported `__int128` from
  `media_time_to_sample_index()`.
- Preserve exact integer `mediaTime -> sample index` conversion using
  quotient/remainder decomposition and 32-bit sample-address wrap semantics;
  no floating-point timing conversion is introduced.
- Verified the real trace conversion
  `28019334008224/1000000000 @ 44100 Hz -> 1235652629` exactly.

# v4.1.79-apap-pipeline (based on v4.1.78-apap-smoke-anch)

- Promote Buffered APAP from the smoke experiment into the normal audio engine.
  APAP now owns only TCP framing, stream-key authentication/decryption and AAC
  access-unit decoding; decoded PCM is published into the existing timed PCM
  ring instead of writing I2S directly.
- Preserve the proven common playout path: APAP PCM now passes through the same
  EQ, PCM ring, PTP presentation scheduler, tagged I2S path and ppm servo used
  by buffered AirPlay audio.
- Treat APAP `mediaTimeValue/mediaTimeScale` as the sample-domain address. At
  44.1 kHz the observed iPhone timeline is directly sample-addressed; other
  scales are converted with integer rational arithmetic before publication.
- Receiver-chosen `anch` is now stable for a `strt` epoch: choose one PTP anchor
  per `strt`, reuse it for repeated `anch` requests, and install the resulting
  media-sample <-> PTP map in the normal buffered scheduler.
- Add APAP-native `fshb` gating using the 24-bit APAP sequence carried in every
  packet. Deferred ranges wait for `flushFromSeq`; immediate flushes invalidate
  queued PCM and discard APAP frames until `flushUntilSeq`. A later immediate
  transition supersedes the earlier deferred range for the same song change.
- Parse and log `flushFromMediaTimeValue/Scale` and
  `flushUntilMediaTimeValue/Scale`. Sequence is authoritative when present; the
  media values are retained for diagnostics/future no-sequence fallback.
- Accept an authenticated extension-only APAP packet at a flush boundary as a
  valid control/boundary packet rather than an AAC decode failure.
- Keep AAC decoder state across APAP timeline discontinuities and mute the first
  post-boundary PCM access unit while resetting EQ history, matching the existing
  Shairport-style discontinuity recovery policy.
- Remove the smoke-only direct-I2S path, fixed -12 dB attenuation and smoke I2S
  lifecycle. Legacy buffered RTP keeps its existing raw 6 MiB FIFO; APAP uses
  TCP backpressure plus the finite timed PCM ring instead of creating a second
  compressed-media byte FIFO.

# v4.1.78-apap-smoke-anch (based on v4.1.77-apap-smoke)

- Preserve the APAP smoke decrypt/AAC path from v4.1.77.
- Fix stream-specific TEARDOWN: tearing down dedicated type-130 RemoteControl
  no longer stops type-103 APAP audio, MediaDataControl, I2S or PTP.
- MediaDataControl callbacks may return a bplist payload in the encrypted rply.
- Handle `strt` and `anch`; `anch` returns a receiver-chosen PTP/media anchor.
- Initialise the I2S playout backend on the APAP path before smoke playback;
  PCM scratch is kept in PSRAM to preserve internal DMA memory.

# v4.1.77-apap-smoke — 2026-10-01

Experimental APAP audible-payload proof build. This intentionally ignores proper
PTP/anchor scheduling and is only meant to prove that the HomePod-style Buffered
APAP transport contains the expected AAC program audio.

- Keeps the HomePod 27.2 feature/fex profile and APAP + MediaDataControl SETUP.
- Copies the negotiated 32-byte `shk` stream key into the APAP endpoint.
- Implements Apple BufferedAPAP framing (`BE32 total length` + APAP packet).
- Implements the fixed 15-byte APAP header and varint extension terminator.
- Mirrors Apple's `APSAPAPBBufEncode/Decode` crypto split: bytes 0..11 are AAD,
  all 15 fixed header bytes stay clear, and bytes 15..end are protected.
- Probes Apple's ChaCha20-Poly1305 64-bit-nonce explicit-8-byte-nonce form first,
  then its internal-counter form; a candidate is accepted only when Poly1305
  authentication succeeds and the resulting access unit decodes as AAC-LC.
- Feeds successfully decoded stereo PCM directly to I2S at 44.1 kHz, attenuated
  by about 12 dB. No PTP timing, `anch`, seek or flush correctness is claimed.
- Drains I2S completion records in smoke mode so the normal diagnostic ring does
  not overflow during continuous unsynchronised playback.

## v4.1.76-apap-observer

- Keep the exact HomePod 27.2 `features`/`fex` profile, including extended bit 72 (`SupportsBufferedAPAP`).
- Parse `streamConnectionTypeAPAP` and `streamConnectionKeyUseStreamEncryptionKey` from type-103 SETUP.
- Mirror APAP in the SETUP reply with its own `streamConnectionKeyPort` instead of silently substituting the legacy buffered TCP/RTP endpoint.
- Add an isolated APAP TCP observer. APAP traffic is **not** fed to the AAC FIFO until the APTransport framing/stream-key cryptor is proven.
- Recognise Apple's BufferedAPAP package envelope (4-byte BE total length) and APAP's 15-byte header when visible; otherwise log the raw prefix to identify the outer encryption/framing layer.
- Leave the working Apple TV `RTP + MediaDataControl` path unchanged.

# v4.1.75-dual-control

- Restores `SETRATEANCHORTIME` to the RTSP `OPTIONS/Public` list. Direct
  iPhone 27.2 captures use an RTP-only buffered stream (`MDC=0`) and the
  RTSP `SETRATEANCHORTIME` + `FLUSHBUFFERED` timeline-control dialect.
- Keeps the HomePod/Apple-TV negotiated MediaDataControl path unchanged:
  when `streamConnectionTypeMediaDataControl` is present, encrypted `srat` /
  `fshb` remain the timeline-control path.
- Does not restore the speculative RTSP `SETRATE` / `GETANCHOR` experiment.
- Adds a SETUP log line showing the buffered control dialect actually selected
  by the sender.

# v4.1.74-mdc-fshb-order

- Keeps the working HomePod-27 MediaDataControl `srat` path unchanged.
- Fixes immediate `fshb` arbitration for the ordering observed from tvOS 27.2:
  a concrete `flushUntilSeq` followed by one or more `flushUntilSeq=0` marker
  requests no longer loses the concrete raw-FIFO boundary.
- This is an ESP buffering adaptation, not a claim that Apple's internal SBAR
  stores the same state: Apple resets downstream queues per flush, while this
  receiver may already hold megabytes of mixed old/future compressed TCP data.
- A later concrete endpoint still replaces a seq-0 fallback; concrete-to-concrete
  updates remain last-writer-wins.  Added a host regression test using the real
  trace sequence 2757133 -> 0 -> 0.
- Removes the old experimental RTSP `SETRATE` / `GETANCHOR` dispatch and its
  per-connection receiver-anchor state.  Those verbs were inferred from Apple
  internal names, not observed on this HomePod-27 wire trace.
- `OPTIONS` no longer advertises legacy `SETRATEANCHORTIME`; its existing handler
  remains only as an unadvertised compatibility fallback if Apple explicitly
  sends it.  Negotiation is therefore driven by the exact HomePod feature mask
  and the observed MediaDataControl transport.

# v4.1.73-mdc-fshb (based on v4.1.72-mdc-srat)

v4.1.72 log: HomePod/Apple TV group plays with the MDC `srat` anchor
(sync within +-0.7 ms); pause is `srat {rate=0}`, then one or more
`sync/fshb {flushUntilSeq, flushUntilTS}`, then `amsm` and a new `srat`.
Later `srat` can also carry firstAudibleMediaTimeValue/Scale (logged only).

- MDC `fshb` goes through the same code as RTSP FLUSHBUFFERED
  (`apply_flushbuffered()`, shared; logs prefixed `MDC fshb`).

# v4.1.72-mdc-srat (based on v4.1.71-datastream-keys)

v4.1.71 log: DataStream keys now confirmed on the first frame (default
derivation, full 64-byte IKM) for RemoteControl and MediaDataControl.
What Apple sends there:
- RemoteControl (type 130, iPhone): `sync/comm` with an MRP
  DEVICE_INFO_MESSAGE (type 15). We only ack it (empty rply) so far.
- MediaDataControl (HomePod as sender, Apple TV group): `sync/amsm`
  {audioMode="default"} and then `sync/srat` {rate, networkTimeTimelineID,
  networkTimeSecs, networkTimeFrac, rtpTime}: the rate/anchor that used to
  come as the RTSP SETRATEANCHORTIME now comes over MDC.

Change:
- `rtsp_datastream_start()` takes a message callback (called from the
  DataStream task for every fully captured message, before the rply).
- MDC `srat` goes through the same code as SETRATEANCHORTIME
  (`apply_rate_anchor()`, shared; RTSP handler only adds the 200 OK). Log
  lines are prefixed `MDC srat`. `amsm` is logged/acked only; any other MDC
  command is logged as `MDC <type>/<cmd>: no handler yet`.

# v4.1.71-datastream-keys (based on v4.1.70-homepod)

Fix: every DataStream (type 130 RemoteControl and the MediaDataControl
connection) failed on the very first frame ("DataStream decrypt/
authentication failed") and the sender tore the stream down.

- Cause: after *transient* pair-setup (what iPhone and HomePod use) the
  Control/Events keys are derived from the full 64-byte SRP session key, but
  the DataStream keys were derived from `shared_secret`, a 32-byte truncated
  copy. The session now keeps the full pairing IKM (`pairing_secret`, 64 B
  after pair-setup, 32 B after pair-verify) and DataStream uses it.
- Diagnostics (no masking): if the first frame still does not authenticate,
  the receiver logs the frame header bytes and tries the other plausible
  derivations (keys swapped, 32-byte IKM, signed seed in the salt); if one
  matches it logs `DataStream keys matched variant N (...)` and uses it, so
  the next log tells exactly which derivation Apple uses.
  `DataStream keys confirmed` is logged on the first good frame.

# v4.1.70-homepod (based on v4.1.68-rt2)

ESP presents itself exactly as a HomePod mini on software 27.2 does
(Discovery capture of "HomePod Withe R", 2026-09-30). Nothing the HomePod
does not advertise is advertised.

- Versions: srcvers/vs and `Server: AirTunes/` 377.40.00 -> **1005.8.1**;
  new osvers/ov 27.2; protovers 1.1; vv 2 -> 1 (TXT, /info, updateInfo).
- Features: new menuconfig "AirPlay identity (HomePod mini)" with
  `AIRPLAY_FEATURES_HOMEPOD_MINI` (default y) = exactly
  features=0x4A7FCA00,0x3C356BD0, fex=AMp/StBrNTwQoa7YDQ (bit 47 gone; 15,
  16, 17, 21, 25, 27, 36, 39, 43, 45, 50, 53, 61 and the extended bits on).
  The per-bit menu remains as "AirPlay feature bits (custom)" when it is off.
- _airplay TXT now: acl, btaddr, c, cmv, deviceid, features, fex, flags,
  gcgl, gid, [gpn], igl, model, osvers, pi, pk, protovers, psi, srcvers, vv.
  pi/psi/gid are stable UUIDs from the MAC (psi begins with the device ID,
  as on HomePods). Group/stereo keys (pgcgl, pgid, tsid, tsm) not published.
- _raop TXT: ek removed, et=0,3,5 (no RSA), ov added, vv=1.
- Status flags: `AIRPLAY_STATUS_FLAGS` (default 0x98004 = bits every HomePod
  shows, without HomeKit bit 10 and group bits) in TXT flags, sf, /info.
- `GET /info` with qualifier txtAirPlay also returns txtAirPlay; pi and
  statusFlags in /info and updateInfo are the real values (were zeros / 4).
- Trace: our own `GET /info reply` and `SETUP reply` bodies; the buffered
  UDP control port watcher (`ap2_ctrl`) is back; every _airplay TXT key is
  logged once at boot.
- Protocol trace options moved to their own menu "AirPlay protocol trace".

# v4.1.68-rt2 (based on v4.1.68-rxdiag-rt)

- Fix: after the first ALAC (realtime) session an AAC SETUP failed -
  `buffered start failed: cannot create processor task (6144 B stack);
  internal free=4863 largest=3584`. The realtime receiver's four queues are
  created once and kept; with xQueueCreate they lived in internal RAM
  (`resend_event_q` alone 512 x 12 B), about 7 KiB taken for good by the
  first ALAC session. They are now created with xQueueCreateWithCaps in
  PSRAM (tasks only, never an ISR).
- What stays in internal RAM after the first ALAC session: the four task
  control blocks (~1.4 KiB) of the persistent realtime tasks.

# v4.1.68-rxdiag-rt (based on v4.1.68-rxdiag)

- Fix: realtime ALAC stream (type 96) could not start while another RTSP
  connection was open - `SETUP: audio receiver start failed: ESP_FAIL`,
  `MEM at SETUP failure internal free=10KiB largest=7KiB`. Each realtime
  SETUP created four tasks with ~18 KiB of internal-RAM stacks. The four
  tasks are now created once (first realtime start) with PSRAM stacks and
  stay for the life of the device; a session wakes them with a task
  notification and each releases its `live_tasks` slot at the end, so
  start/stop semantics are unchanged. No per-SETUP task creation or deletion.
- Memory logging: `New client connected (internal free/largest/min)`,
  `MEM after stream SETUP`, `MEM at TEARDOWN`, to find what takes internal
  RAM down (a log showed min=1KiB).

# v4.1.68-rxdiag (based on v4.1.68-merge)

- Diagnostic only, for the "sender stops sending" stall (seen after a seek
  on the iPhone and after AutoMix on the Apple TV: `rx=0KB/s` until the FIFO
  runs dry). `rx=0` alone cannot tell whether the sender stopped or our TCP
  reader stopped reading and let the TCP window fill.
- The buffered TCP reader now records where it is (`accept`, `recv`,
  `wait-space`, `pace`), since when, and when the last byte arrived.
- The audio_status task logs, every 2 s while the sender has sent nothing for
  3 s and the FIFO holds less than 1 MiB:
  `RX IDLE <ms>, fifo=<KiB>: reader=<state> for <ms>, socket unread=<B>, recv timeouts=<n>`
  and `RX resumed ...` once bytes arrive again. `reader=recv` with
  `socket unread=0` means lwIP has nothing for us: the sender is not sending.
  Normal sender pauses happen with a full FIFO and are not reported.
- Host test T8 (idle sender -> reader in recv, 0 unread).
- No audio, FLUSH, PTP or RTSP behaviour changed.

# v4.1.68-merge (based on v4.1.68-readable-rtsp-control-trace)

- v4.1.66 -> v4.1.68 were built without v4.1.65 (below): the second-sender
  PSRAM fallback, SETRATE/GETANCHOR and the faster DataStream stop are merged
  back. Connection model, timeline-control split and readable RTSP trace from
  v4.1.67/v4.1.68 are kept unchanged.
- `PROJECT_CONTRACT.md` and `tools/` (host tests), missing from the v4.1.66 -
  v4.1.68 archives, are restored.
- Build warnings fixed: `trace_hash32` unused with the trace on but
  MR_COMMANDS off, and `TAG` unused with the trace off.

# v4.1.65 (based on v4.1.64-readable-metadata-trace, parallel to v4.1.65-mediaremote-state)

Merges the fixes from the parallel v4.1.59-v4.1.61 line (see their entries
below) into the DataStream / protocol-trace line:

- Second sender while playing (was v4.1.59): v4.1.64 log shows the old
  failure again - iPhone connects while the Apple TV plays, `Failed to create
  client task`, power cycle needed. RTSP client tasks fall back to a PSRAM
  stack when internal RAM is short; device name and volume are cached in RAM
  and the volume is saved by a short-lived task with an internal stack, so
  the RTSP path never touches flash.
- SETRATE / GETANCHOR (was v4.1.60/61): iOS 27.2 iPhone asks the receiver to
  place the anchor. Placed at once with a locked, qualified PTP GM, otherwise
  on any valid estimate 1.5 s after SETRATE. Works together with the
  play-lock PTP reset from v4.1.60 (DataStream line), which keeps the PTP
  samples collected after SETPEERS.
- Trace: GETANCHOR (polled ~10/s) is traced once per SETRATE; `GETANCHOR:
  waiting ...` (at most 1/s) and `GETANCHOR: anchor placed ...` show the rest.
- DataStream stop: the task now polls every 100 ms instead of 1 s, so
  SETUP/TEARDOWN replies are no longer held up to ~1 s by
  `rtsp_datastream_stop()`; stop waits up to 1 s before keeping the context.

Note: the two lines both used the numbers v4.1.60 and v4.1.61 for different
content. Both sets of entries are kept below; from here on there is one line.

# v4.1.68-readable-rtsp-control-trace

- Replaces the long one-line bplist trace for `SETUP`, `SETPEERS/SETPEERSX`,
  `SETRATEANCHORTIME` and `FLUSHBUFFERED` with compact structured multi-line
  blocks.
- Initial `SETUP` is grouped into Sender, Session, Timing and Capabilities;
  stream `SETUP` shows the negotiated stream type/codec/frame size, dynamic
  stream ID, stream connection model and crypto-field sizes.
- `SETPEERSX` prints each parsed peer's clock identity and addresses instead
  of a single large dictionary line. `SETRATEANCHORTIME` and
  `FLUSHBUFFERED` show their timeline boundaries as named fields.
- Adds `AIRPLAY_PROTOCOL_TRACE_RAW_BPLIST` (default off). Enable it only when
  the exact raw bplist description is needed in addition to the readable
  structured trace.
- Logging only: no RTSP response, pairing, connection negotiation, PTP,
  FLUSH, MediaRemote or audio behaviour is changed.

# v4.1.67-connection-timeline-split

- Separates AirPlay connection/setup parsing from media timeline parsing without changing audio behaviour.
- Adds `rtsp_connection_model.c/.h` for negotiated stream transport fields: codec/rate/frame-size defaults, sender control port, `supportsDynamicStreamID`, `streamConnections`, RTP/RTCP/MediaDataControl and its seed.
- Adds `rtsp_timeline_control.c/.h` for typed parsing of `FLUSHBUFFERED` and `SETRATEANCHORTIME`, including the existing Apple 32.32 network-time conversion.
- `rtsp_handlers.c` remains the orchestrator and still performs all existing audio/PTP side effects in the same order. Metadata and connection negotiation still cannot drive the audio timeline.
- No FIFO, decoder, PTP servo, anchor application, FLUSH semantics, playout scheduling, pairing or MediaRemote behaviour is intentionally changed.

# v4.1.65-mediaremote-state

- Adds a real, persistent receiver-side MediaRemote state: Now Playing identity,
  timeline/rate context, transition flags, artwork descriptors, reported
  playback state and the sender's supported/enabled command set.
- `updateMRNowPlayingInfo` understands `mergePolicy=replace` versus sparse
  `mergePolicy=update`. Timeline-only seek/pause/resume updates therefore do not
  erase the track identity from the internal state.
- Tracks item changes separately from ordinary progress/timeline updates. This
  is the foundation for distinguishing seek/scrub, next/previous and AutoMix
  transitions in the next step without allowing metadata to drive audio.
- Parses `updateMRSupportedCommands` into a 256-command present/enabled bitmap,
  including the seek/scrubbing capability options. Common Apple command IDs are
  named for diagnostics (play/pause/next/previous/seek/FF/rewind/repeat/shuffle).
- Existing DMAP/progress metadata is retained as a fallback only. Once
  MediaRemote provides a field it remains authoritative for that field; a late
  DMAP packet from a previous title cannot overwrite a new MediaRemote item.
- Keeps two playback concepts deliberately separate: MediaRemote's reported
  playback state is context, while `audio_transport_playing` comes from the
  existing RTSP audio events. The raw numeric MediaRemote state is retained too,
  so newly observed values can be correlated later without guessing their
  meaning. Neither MediaRemote metadata nor capabilities can start, stop, flush
  or re-anchor the audio pipeline.
- Full protocol tracing remains available through `AIRPLAY_PROTOCOL_TRACE`, but
  is now off by default. Normal MediaRemote state works without the heavy trace.
- No FLUSHBUFFERED, SETRATEANCHORTIME, D7/PTP, FIFO, decoder, DSP or playout
  scheduling behaviour is changed in this first part.

# v4.1.64-readable-metadata-trace

- Formats `updateMRNowPlayingInfo` into a compact human-readable `PTRACE NOW PLAYING` block: title, artist, album, time/duration, playback rate, transition state, queue/track position, genre/composer, collection/source, artwork dimensions/size and stable identifiers.
- The large artwork DATA payload is never rendered into the log; only its byte size is reported.
- Suppresses the redundant one-line raw bplist dump for `updateMRNowPlayingInfo` and `updateMRSupportedCommands` because both now have dedicated structured trace output.
- Adds bounded deep-search helpers for nested bplist string/int/real/bool values and DATA lengths, retaining the existing recursion depth and object-visit limits.
- Adds a host regression test for nested MediaRemote metadata, including a 135106-byte artwork DATA object.
- Diagnostic only: no RTSP response, audio, PTP, FLUSH, FIFO, codec, DSP or playout behavior changed.

# v4.1.63-mediaremote-commandinfo-trace

- `updateMRSupportedCommands` tracing now opens every nested serialized
  `CommandInfo` bplist in `mrSupportedCommandsFromSender` instead of showing
  only `<data N>` placeholders.
- Each command entry logs its array index, numeric `kCommandInfoCommandKey`,
  `kCommandInfoEnabledKey`, full bounded options dictionary, payload size and a
  32-bit content hash. This makes 37 -> 33 capability-set changes and
  same-length option mutations directly observable.
- Added a bounded generic bplist helper for reading one DATA item from a named
  nested DATA array. It retains the parser's recursion depth and visit-budget
  protections.
- Diagnostic bplist rendering now keeps long MediaRemote option-key names
  instead of replacing keys longer than the old 47-character scratch buffer
  with `?`.
- Diagnostic only: no RTSP responses, event messages, DataStream behavior,
  audio, PTP, FLUSH, FIFO, codec, DSP or playout state is changed.

# v4.1.62-mediaremote-full-trace

- Protocol trace now logs every RTSP request occurrence so late feature-bit reactions are not hidden by method/path de-duplication.
- Event-channel trace is no longer capped at the first 16 decrypted messages.
- Logs exact effective `features` and `fex` masks plus all set bit numbers at startup.
- `POST /command` now emits a dedicated `PTRACE /command type=...` summary while retaining the full bplist dump.
- DataStream trace now logs every message and captures the first 16 KiB even for large payloads.
- DataStream/`params.data` diagnostics recognize length-prefixed MediaRemote `ProtocolMessage` envelopes and report message type, identifier and extension field numbers without adding a protobuf runtime.
- Diagnostic-only change: no audio, PTP, FLUSH, FIFO, codec, DSP or playout behavior is changed.

# v4.1.61-automix-loudness-trace

- Diagnostic-only AutoMix/AirPlay 2 change.
- `LOUDNESSNORMALIZATION` is no longer protocol-trace de-duplicated: every
  request body is rendered with the existing bounded `bplist_describe()` path.
- Handler log now includes `changed=0/1` and bplist body length.
- No DSP, PTP, FLUSH, FIFO or playout behaviour changed.

# Changelog

Newest first. Architecture rules live in `PROJECT_CONTRACT.md`; this file only
records what changed in each version and why.

## v4.1.60 (based on v4.1.58 DataStream branch) - PTP session lifecycle

- Moves the one clean PTP estimator reset to **play-lock acquisition**, the
  actual AirPlay control-session boundary. This happens before the sender's
  SETPEERS/SETPEERSX traffic can build the session estimator.
- The first audio stream SETUP no longer throws away PTP samples collected
  between SETPEERS and SETRATEANCHORTIME. This targets the observed startup
  failure where the estimator was reset after SETPEERS, then remained at one
  stale sample until the 6 s timing watchdog reset it.
- Keeps a defensive stream-SETUP fallback reset only for synthetic/direct
  handler paths that somehow bypass normal play-lock acquisition.
- RemoteControlOnly/type-130 sessions still never touch the global PTP clock
  because they do not acquire the audio play lock.
- No FLUSH, FIFO, decoder, servo or playout scheduling changes.

## v4.1.58 (based on v4.1.57) - AirPlay DataStream / MediaDataControl

- Implements the transport advertised by feature bit 60
  `SupportsAudioMediaDataControl`: parse
  `streamConnectionTypeMediaDataControl` and its 64-bit
  `streamConnectionKeyEncryptionSeed`, open a dedicated TCP listener, derive
  receiver-side DataStream keys from the pair-verify shared secret using
  `DataStream-Salt<seed>` plus the Input/Output encryption infos, and return
  `streamConnectionKeyPort` in the audio SETUP response.
- Adds encrypted AirPlay DataStream framing: HAP ChaCha20-Poly1305 records,
  32-byte big-endian DataStream headers, bounded binary-plist tracing, and
  `sync -> rply` acknowledgement with the matching sequence number. Large
  payloads are consumed without retaining the whole body in RAM.
- Adds the same dedicated DataStream transport for stream type 130 when the
  sender supplies a `seed`, returning `{type=130, streamID=1, dataPort}`. This
  implements the transport layer; full MediaRemote/MRP command semantics are
  not claimed yet.
- `LOUDNESSNORMALIZATION` is now an explicit method: the requested enable flag
  is parsed and stored instead of falling through the unknown-method handler.
  DSP loudness normalization is intentionally unchanged.
- Bit 60 is enabled by default now that its transport is implemented. Bits 58
  and 61 remain off by default until their complete receiver behavior is
  implemented.
- No PTP-lifecycle, FLUSH/FIFO, codec scheduling or playout changes in this
  release, so protocol experiments remain isolated from the timing fixes.

## v4.1.57 (based on v4.1.56) - AirPlay streamConnections setup

- Implements the receiver side of feature bit 59
  `SupportsAudioStreamConnectionSetup` for audio SETUP: parse the sender's
  `streamConnections`, return a receiver-assigned 32-bit `streamID`, and mirror
  requested RTP/RTCP connection types with `streamConnectionKeyPort`.
- Buffered type-103 streams now return a real non-zero UDP `controlPort` and
  keep that socket bound for the stream lifetime, while audio data remains on
  the existing TCP buffered port. This matches the AirPlay 2 setup shape used
  by Shairport/OpenAirPlay instead of advertising `controlPort=0`.
- Feature bit 60 `SupportsAudioMediaDataControl` is now off by default because
  the separate MediaDataControl transport is not implemented. Advertising it
  without the transport made the sender select a protocol path the receiver
  could not complete.
- Added bounded parsing for RTP/RTCP/MediaDataControl connection types and a
  generic binary-plist SETUP builder for the extended response.

## v4.1.56 (based on v4.1.55) - feature-bit experiment

- Model `AudioAccessory5,1` (HomePod mini) instead of HomePod 2.
- New menuconfig menu "AirPlay feature bits (experimental)": one switch per
  feature bit a HomePod mini advertises and we do not (15, 16, 17, 21, 25,
  27, 36, 39, 43, 45, 50, 52, 53, 58, 59, 60, 61 and the extended bits 68-99),
  plus bit 47 (Shairport Sync has it, the HomePod mini does not). Defaults on:
  47 (as before), 52 SupportsSetPeersExtendedMessage, 59
  SupportsAudioStreamConnectionSetup, 60 SupportsAudioMediaDataControl.
  Everything else is off; switching a bit needs only a rebuild, no new code.
  With 59/60 the sender may use a path we do not implement - if audio does
  not start, switch them off.
- Extended bits (64+) are published as the `fex` TXT key when any is on
  (encoding verified against the HomePod mini's own `fex`).
- Protocol trace (menuconfig "Log what the sender sends", default on): the
  first request of each method/path per connection and every SETUP are
  logged with header names and body (bplist rendered as text by the new
  bounded `bplist_describe()`), plus the first 16 event-channel messages from
  the sender. Lines start with `TRACE`.
- Feature-bit comments corrected (38 = SupportsUnifiedMediaControl,
  48 = SupportsCoreUtilsPairingAndEncryption).

## v4.1.55 (based on v4.1.54)

- Device identity back to HomePod 2, as up to v4.1.48: model
  `AudioAccessory6,1`, srcvers / `Server: AirTunes/` `377.40.00`, feature bits
  `0x1C340405C4A00` (bit 46 set again, HomePod presentation in the sender's
  UI). Everything comes from the single defines `AIRPLAY_MODEL`,
  `AIRPLAY_FEATURES_HI/LO` and `AIRPLAY_SOURCE_VERSION`, so mDNS (`_airplay`
  and `_raop` "am"/"ft"), `/info` and the event-channel updateInfo all agree.
- Host test for updateInfo updated to the new identity.
- FLUSH diagnostics (`AIRPLAY_DIAG_FLUSH`, off by default): the
  `FLUSH controlRx ...` line no longer fires every 2 s. It counted the wait
  for the sender's next request (idle time, e.g. /feedback every 2 s) as
  "slow"; now only receiver-side time (payload + decrypt >= 50 ms) is logged.

## v4.1.54 (based on v4.1.53-test) - cleanup release

No new features. Test code, test options and inaccurate comments removed; the
functional code is that of v4.1.53-test except for the points under
"Behaviour" below.

Removed test options and code:
- Kconfig `AIRPLAY_TEST_FLUSH_RESERVE_KIB` and the FLUSH reserve (tests with
  0/16/64/128/200 KiB all reproduced the sender stall).
- Kconfig `AIRPLAY_AP2_ADVERTISED_BUFFER_KIB`: `audioBufferSize` is again the
  physical 6 MiB FIFO, as up to v4.1.46.
- Kconfig `AIRPLAY_MODEL` (model fixed to `ShairportSync`),
  `AIRPLAY_EVENT_UPDATE_INFO` (updateInfo always sent),
  `WIFI_PREFER_5GHZ` / `_MIN_RSSI` (never read by any code),
  `AIRPLAY_DIAG_HIGH_RATE_TRACE` (only changed a banner line).
- Stack high-water and MEM test logs (RTSP client, event task, buffered
  processor, OTA, Wi-Fi scan, boot stages). Kept: one memory summary at
  "ready" and the memory state in audio-start failure logs.
- Event-channel logs of every sender reply; updateInfo "sent" is debug level.
- Dead code: AirPlay 1 SDP/RSA/NTP remnants (AirPlay 1 SETUP was already
  rejected with 461), `CONFIG_AIRPLAY_FORCE_V1` branches, DHCP option 114
  code (was disabled), unused functions (wifi_stop, web_server_stop, several
  ptp_clock getters, hap_encrypt/decrypt, base64_decode, plist_xml helpers,
  led error/brightness API, settings Wi-Fi getters, unused audio_playout /
  latency_cal / rtsp_conn helpers, unused diag hooks), unused fields, macros
  and includes.
- Comments: version-history notes ("v4.1.x: ...") rewritten as plain
  statements or removed; comments that did not match the code corrected.

Behaviour:
- updateInfo is built in the RTSP task when the event port is created, not in
  the event task: the event task's stack is in PSRAM and must not read NVS.
- AirPlay 1 style SETUP / ANNOUNCE no longer takes the play lock, so such a
  request is answered 461 without stopping the current playback.
- The RGB LED goes to standby when the playing connection disconnects
  (it used to keep showing playing/paused).
- The setup AP uses Kconfig `DEFAULT_AP_CHANNEL` (default 1, as before; the
  option was ignored until now).
- The RGB LED build without the VU mode compiles without unused-code warnings.
- Web UI text: EQ changes apply live (index page said "after restart"); the
  PCM lead is about 0.85 s (EQ page said ~1.1 s).
- A few log levels adjusted (unknown method and FLUSHBUFFERED immediate
  untilSeq=0 are info, play-lock details are debug).

## v4.1.53-test (based on v4.1.52-test) - TEST BUILD, not for normal use

- Fix: RTSP client stack nearly overflowed. A device log showed `stack min
  free 100 of 8192 B` on disconnect. Static analysis (-fstack-usage /
  -fcallgraph-info) found SETPEERS/SETPEERSX: its parsed peer list
  (`bplist_peer_info_t[16]`, ~4.4 KiB) was a local array, giving a worst
  path of ~8.5 KiB on an 8 KiB stack. The list is now allocated in PSRAM for
  the duration of the request; if that fails the previous peer list is kept
  and 200 OK is returned as for other unusable bodies.
- FLUSH reserve default is now 0 (off). Tests with 0, 16, 64 and 128 KiB all
  reproduced the sender stall with the same pattern: a second seek shortly
  after the previous one, the FLUSH target lies ahead of what the sender has
  sent, the sender pushes part of that region and stops (rx=0) while RTSP
  stays alive. The earlier 200 KiB runs without a stall are taken as chance;
  nothing the sender can observe differs between 64/128 and 200 KiB.
- Otherwise identical to v4.1.52-test.

## v4.1.52-test (based on v4.1.51-test) - TEST BUILD, not for normal use

- FLUSH reserve without a timeout: discarding stops when fewer than
  `AIRPLAY_TEST_FLUSH_RESERVE_KIB` (default 200) are left in the FIFO. It
  continues only as new data arrives, until the FLUSH reaches a block that is
  played. The 2 s escape of v4.1.51-test is removed; it drained only one block
  per 2 s and could itself hold a FLUSH endpoint back.
- One log line per FLUSH: `TEST reserve 200 KiB: FLUSH discard paused at N KiB
  in FIFO` (was one line per block).
- Plan: repeat the AutoMix + seek + fast scrub recipe with 200, 150, 100 and
  50 KiB.

## v4.1.51-test (based on v4.1.50) - TEST BUILD, not for normal use

- Kconfig `AIRPLAY_TEST_FLUSH_RESERVE_KIB` (default 200 in this build only).
  While a FLUSH is draining, discarding pauses when fewer than 200 KiB are
  left in the FIFO and resumes when more data arrives. After 2 s without data
  the rest is drained. A seek therefore never drains the FIFO empty. It tests
  whether the HomePod/Apple TV sender stall (quick seeks after AutoMix)
  depends on the receiver's buffer running empty.
- Side effect: audio after a seek starts ~1 s later (200 KiB at ~190 KB/s).
- Log: `TEST reserve: FLUSH drain paused with N KiB in FIFO`, `TEST reserve:
  no data for 2000 ms, draining the last N KiB`.
- Not in PROJECT_CONTRACT.md on purpose: it is an experiment. Normal builds
  must set it to 0 or remove it.

## v4.1.50 (based on v4.1.49)

Verified: full ESP-IDF 5.5.2 build, 0 warnings, default and all-diagnostics
configs. Host tests pass. The firmware image no longer contains `377.40`.

- The AirPlay software version is now Shairport Sync's `366.0` (was
  `377.40.00`, inherited from rbouteiller, which has the same sender stall).
  It is defined once in `rtsp/airplay_version.h` and used for mDNS `srcvers`
  (and RAOP `vs`), `/info` and the `Server: AirTunes/366.0` header of every
  RTSP response. Senders choose features by this version. Together with
  v4.1.49 (model `ShairportSync`, features `0x18340405C4A00`), the device now
  presents exactly the same identity as Shairport Sync.

## v4.1.49 (based on v4.1.48)

Verified: full ESP-IDF 5.5.2 build, 0 warnings, default and all-diagnostics
configs. Host tests pass. The firmware image no longer contains any
`AudioAccessory` string.

### Present as an ordinary AirPlay speaker, like Shairport Sync

- `model` was `AudioAccessory6,1`, the HomePod (2nd gen) identifier;
  rbouteiller uses `AudioAccessory5,1`, HomePod mini. Both ESP projects have
  the sender stall that appears only with HomePod/Apple TV sources. Shairport
  Sync reports `ShairportSync`; in its source `AudioAccessory5,1` is commented
  out. Now the default is `ShairportSync`, in mDNS (`model`, RAOP `am`),
  `/info` and `updateInfo`. It is Kconfig `AIRPLAY_MODEL`, so the old
  behaviour can be restored for A/B tests.
- Feature bit 46 (SupportsHKPairingAndAccessControl) removed: features are now
  exactly Shairport's `0x18340405C4A00` (was `0x1C340405C4A00`).
- Visible effects: a different icon and type in the Home app / AirPlay list.
  The device may need to be removed and added again in Home, and the senders
  may cache the old identity until they see the new mDNS record (restart the
  HomePod/Apple TV if it still shows the old one).

## v4.1.48 (based on v4.1.47)

Verified: full ESP-IDF 5.5.2 build, 0 warnings, default and all-diagnostics
configs. Host tests pass; the FLUSH test now also replays the 146 s case from
the first v4.1.47 log.

### untilSeq=0: timeline window measured against the anchor

- In the first v4.1.47 log the new-timeline rule rescued a real seek. The
  untilTS marker never came because the sender re-based its RTP clock, and
  v4.1.46 would have stayed silent there until the sequence wrapped.
- The same log showed the first new block 1.6 s ahead of the playout clock,
  against a v4.1.47 limit of +3 s. That window was measured against the clock:
  had the sender placed its anchor a little further in the future, every new
  block would have been "too early", dropped, and the flush would never have
  ended. The window is now +/-10 s around the new anchor's RTP, independent of
  wall-clock time and of how fast the blocks arrive.

### Internal RAM

- The v4.1.47 log shows only 19 KiB internal RAM free (largest block 12 KiB)
  at session start, before the 6 KiB AAC processor stack is taken. A second
  RTSP connection (8 KiB stack) is then enough to make the next stream SETUP
  fail. That fits the SETUP failures seen earlier.
- The event-port task stack (6 KiB) now lives in PSRAM. The task never touches
  flash, so this is safe. RTSP client tasks stay in internal RAM because they
  write NVS (volume).
- Stack high-water marks are logged when the RTSP client, event port and AAC
  processor tasks end (`stack min free N of M B`). The next logs will show how
  far those stacks can be reduced safely.
- `MEM stream start …` is logged after the processor task is created.

## v4.1.47 (based on v4.1.46)

Verified: full ESP-IDF 5.5.2 / GCC 14.2 build, 0 warnings, default config and
all diagnostics + `AIRPLAY_LATENCY_CAL`. Host tests pass, including the new
`test_flush.c`. It runs the real FLUSH classifier, extracted by
`extract_flush.py`, on block sequences taken from HomePod logs.

### Immediate FLUSH with untilSeq=0 (ESP deviation from Shairport)

- Before: `flushUntilSeq=0` was compared modulo 2^23, as Shairport does. When
  the session's sequence numbers are above 4 194 304 (2^22), every block was
  discarded until the sequence wraps. That meant minutes to hours of silence
  while the sender kept streaming (status line `flush=0`, `rx>0`, `fifo=0`).
  Whether it happened depended only on the random starting sequence number of
  the session.
- Now the flush ends at the block whose RTP == `flushUntilTS`. In every
  HomePod log so far that is exactly the first block of the new content, the
  same block that ends the deferred ranges. If that block never comes, the
  flush ends at the first block on the new anchor's timeline (-10 s ... +3 s
  around the RTP wanted now). A later valid immediate FLUSH still replaces
  it.
- New log lines:
  - `FLUSHBUFFERED immediate: untilSeq=0 untilTS=… -> ends at the untilTS
    block or on the new anchor timeline`
  - `AAC FLUSH complete at untilTS marker …`
  - `AAC FLUSH complete on new anchor timeline …`
- Status line: `flush=ts:<rtp>` while armed.

### SETUP failures: logged, recovered

- Observed: after a long session every new stream SETUP answered 500 without
  any log. The sender tore down and retried forever until a power cycle.
- Every silent failure path in the buffered start now logs its reason:
  realtime receiver still active, engine not initialised, or processor task
  not created (with heap numbers).
- A failed buffered start resets the audio engine (`audio_receiver_stop()`,
  re-init if needed) and retries once.
- After 3 failed stream SETUPs in a row the device restarts itself.
- `MEM session start …` is logged at every session start (internal/PSRAM
  free, largest block, minimum), so a slow leak or fragmentation becomes
  visible over hours.

### audioBufferSize

- The advertised buffered capacity is now Kconfig
  `AIRPLAY_AP2_ADVERTISED_BUFFER_KIB`, default 7168 KiB (was fixed 6144;
  Shairport uses 8192). The physical FIFO stays 6 MiB; when it is full, TCP
  backpressure holds the sender.

## v4.1.46 (based on v4.1.45)

Verified: full ESP-IDF 5.5.2 / GCC 14.2 build, 0 warnings, default config and
all diagnostics + `AIRPLAY_LATENCY_CAL` with `AIRPLAY_EVENT_UPDATE_INFO` off.
Host tests pass, including two new ones:
- `test_update_info.c`: the plist decoded with Python plistlib;
- `test_event_channel.c/.py`: the real key derivation and framing against an
  independent Python sender model (HKDF-SHA512 + ChaCha20-Poly1305, Shairport's
  key orientation), in both directions.

`GET /info` output is byte-identical to v4.1.45.

### Event channel: updateInfo (Shairport parity)

- When the sender connects to the event port, the receiver now sends one
  encrypted `POST /command` with `{type = "updateInfo"; value = <the /info
  dict> + txtAirPlay}`, exactly as Shairport Sync `ap2_event_receiver.c` /
  `ap2_event_send_update_info()` does. `txtAirPlay` is the same TXT record we
  advertise over mDNS (one shared source now).
- Event channel keys are derived with the control keys at pairing
  ("Events-Salt"; the receiver writes with "Events-Write-Encryption-Key" and
  reads with "Events-Read-Encryption-Key", like Shairport's server cipher
  channel 4). The transient pairing path uses the full SRP key, as for the
  control channel.
- The sender's answer is decrypted and logged:
  `Event channel from sender: RTSP/1.0 200 OK`.
- Kconfig `AIRPLAY_EVENT_UPDATE_INFO` (default on) turns it off for A/B tests.

### Event port busy-loop fix

- The event task only `MSG_PEEK`ed incoming data. Any byte from the sender
  (for example the answer to updateInfo) stayed in the socket, so `select()`
  returned immediately forever. The task (priority 5, core 0) then spun and
  starved the buffered audio processor (priority 4, core 0) and IDLE0. That
  would give a full FIFO, then a zero TCP window, then the sender stopping
  and silence. rbouteiller's code has the same pattern. Everything received
  is now consumed. Task stack 3 -> 6 KiB for the crypto.

### New log lines

- `Event channel: sent updateInfo (N bytes)`
- `Event channel from sender: <first line> (N bytes)`
- `Event client disconnected (N messages, M bytes discarded)`
- `Event channel: sender sent N bytes we cannot decrypt (discarded)`

## v4.1.45 (based on v4.1.44)

Verified: full ESP-IDF 5.5.2 / GCC 14.2 build, 0 warnings, default config and
all diagnostics + `AIRPLAY_LATENCY_CAL`. Host tests pass, including the new
`test_rtsp_server.c` (the real `rtsp_server.c` over loopback TCP).

### Shairport Sync connection model (play lock)

Before: every new RTSP connection stopped the running session
(`Waiting for old client slot...`). A HomePod/iPhone opening a second
connection for `/info`, pairing, remote control or multiroom coordination
therefore killed playback, and the sender could stay silent afterwards.

Now, as in Shairport `rtsp.c` (`principal_conn`, `get_play_lock()`):

- Every connection has its own client task (up to 3). A new connection does
  not touch audio.
- Only one connection, the one holding the play lock, drives audio, PTP,
  volume, amplifier and event port. A connection takes the lock when it starts
  playing (initial SETUP with timing, stream SETUP, AP1 ANNOUNCE/SETUP). The
  previous owner is stopped first, and the new one waits until its cleanup
  has finished.
- Initial SETUP with `timingProtocol: None` (remote control only) is answered
  without taking the lock and without touching playback.
- Playback commands on a connection without the lock are answered 200 with no
  effect. The volume is remembered and applied if that connection starts
  playing.
- All slots busy: the oldest non-playing connection is closed, never the
  player.
- Handlers of different connections are serialised by one mutex (shared
  parse/response buffers, lazy RSA init, audio state).

### Small parity fixes

- Unknown RTSP method: `200 OK` like Shairport (was `501`).
- Unparsable request: `400 Bad Request` (was no reply).
- Removed an empty DACP block from the dispatcher.

### New log lines to look for

- `Client slot N acquired the play lock` / `takes the play lock from slot M`
- `<METHOD> on a connection without the play lock: acknowledged`
- `Client slot N disconnected (was playing)`
- `All 3 RTSP slots busy; closing idle slot N`

## v4.1.44 (based on v4.1.43-zero-seq-warn)

Verified: full ESP-IDF 5.5.2 / GCC 14.2 build of the firmware, 0 warnings, both
with the default `sdkconfig.defaults` and with every `AIRPLAY_DIAG_*` option
plus `AIRPLAY_LATENCY_CAL` enabled. Host tests (ASan/UBSan) for the FIFO over
real loopback TCP, the bplist parser (regression corpus, crafted attacks,
1 M-iteration mutation fuzz) and the output volume/dither. They are now in
`tools/host_tests/` (run `tools/host_tests/run.sh` on Linux/macOS; not part of
the firmware build).

### Buffered FIFO / TCP / FLUSH (speed)

- **Dropped blocks are no longer copied.** The processor now reads a block in
  two steps: the 12-byte RTP head first (`ap2_buffered_fifo_read_block_head`),
  then the body (`ap2_buffered_fifo_read_block_rest`). When FLUSH, an
  unrecognised SSRC or an unsupported format drops the block at the head, the
  body is consumed without a memcpy out of PSRAM. A seek/next-track drain of a
  large backlog (up to 6 MiB) therefore costs roughly a pointer move per block
  instead of copying every discarded byte. Still exactly one sequential
  consumer: no search, no skip-ahead, no second cursor (contract §3/§6).
- **One lock for length + head.** When the length prefix and the head are
  already queued (the normal case) they are taken under a single mutex hold.
- **Fewer control-mutex round trips while draining.** A block dropped by FLUSH
  lets the next read skip the extra `buffered_control_must_drain()` check; the
  per-block FLUSH classification is unchanged.
- **Corrupt length prefix is detected before waiting for the head.** Previously
  a short/invalid prefix could wait for bytes that never come.
- **Orderly close keeps the backlog (Shairport parity).** When the sender
  closes the buffered TCP connection (FIN), bytes already received stay
  playable until consumed, as Shairport's `buffered_read()` does. Before, the
  whole unplayed backlog (up to ~2 minutes of AutoMix preload) was discarded
  immediately. Stop/abort/errors and a new accepted connection still discard.
- Removed the write-only `total_read` counter.

### Security (regression from v4.1.40 restored)

- The v4.1.40 bplist hardening was missing in v4.1.43. Restored on top of the
  v4.1.43 parser changes: overflow-free span checks (32-bit `size_t` on the
  ESP32 let `pos + count * ref_size` wrap -> out-of-bounds read / panic),
  8-byte length limit, and a visit budget for the recursive key search
  (self-referencing dict -> endless loop). Reachable before pairing via
  `/command`, `/feedback`, SETUP.
- Restored the RTSP response header size guard in `rtsp_message.c`.
- Plain `SETPEERS` now skips a non-string entry and keeps the valid addresses,
  exactly like `SETPEERSX` `Addresses` (Shairport `handle_setpeers()`).

### AirPlay command parity with Shairport Sync 5.5.2

- RTSP control socket now uses TCP keepalive like Shairport (95 s idle,
  5 probes x 5 s). A sender that vanishes without FIN/RST ends the session
  after ~2 minutes instead of keeping it (and the amplifier) forever.
- `/feedback` also answers with the stream status plist for an active AirPlay 2
  realtime stream (type 96), as Shairport `handle_feedback()` does for 96 and
  103. Type 103 behaviour is unchanged.
- Reviewed FLUSHBUFFERED (immediate/deferred, 10 deferred slots), SETRATEANCHORTIME
  (anchor on `networkTimeSecs`, rate 0 = pause + anchor reset, no rate = play
  state unchanged) and TEARDOWN (with / without `streams`): already equivalent.

### Output volume and dither

- TPDF dither was already present. It was switched off per individual zero
  sample, which gates the noise floor with the signal on very quiet material.
  Now an all-zero block (silence, pause, track gap) stays bit-exact zero, and a
  block with signal is dithered on every sample.
- Unity gain remains bit-perfect; a fade to mute now ends in exact zeros.
- Volume ramp uses 32-bit hardware division instead of a 64-bit software
  division per frame (Xtensa has no 64-bit divide instruction).
- Tested: mean error 0.000 LSB, RMS 0.50 LSB (ideal for TPDF + rounding),
  no overflow at full scale, ramp matches the previous implementation within
  2 LSB (dither).

### Dead code and comments

- Removed unused `audio_receiver_*` API: `start`, `stop_buffered_only`,
  `get_stream_port`, `get_buffered_audio_buffer_size`, `get_volume_q15`,
  `flush`, `is_playing`, `reset_timing`, `get_hardware_latency_us`.
- Removed `pcm_rtp_ring_invalidate_before()` (unused PCM scan; the contract
  requires O(1) generation invalidation instead).
- `audio_diag.c`: `diag_transport_poll()` had been truncated (missing log and
  closing braces), so any build with `AIRPLAY_DIAG_TRANSPORT` enabled failed to
  compile. Restored.
- Fixed misleading comments (task priorities/cores, playout latency API).
- `sdkconfig.defaults` now ends with a newline (appending options used to join
  the last line).

### Not changed on purpose

- TCP reader pacing (4 KiB recv, 10 ms sleep above 16 KiB) is identical to
  Shairport `buffered_tcp_reader()` and is not the seek bottleneck.
- Immediate FLUSH with `flushUntilSeq = 0` keeps Shairport's modulo-2^23
  semantics (v4.1.43 decision).
- Separate ports (RTSP 7000, event, buffered TCP, realtime UDP, PTP 319/320)
  and separate tasks already match Shairport's topology.

# Parallel line v4.1.59-v4.1.61 (merged into v4.1.65)

## (parallel line) v4.1.61 (based on v4.1.60) - GETANCHOR does not wait for a PTP lock

- v4.1.60 log: iPhone (iOS 27.2) took over from the Apple TV, sent SETRATE and
  polled GETANCHOR, but PTP to the iPhone did not lock within 8 s after the
  switch (jittery first samples, raw vs filter 46 ms). No anchor was placed,
  the sender gave up (FLUSHBUFFERED + TEARDOWN) and on the second attempt
  fell back to SETRATEANCHORTIME, which played.
- The anchor is now placed at once when PTP is locked and qualified, as
  before, and otherwise on any valid PTP estimate 1.5 s after SETRATE. The
  anchor is only a point on the sender's timeline; later offset refinement
  moves our playout, not the anchor. The placement log says `ptp locked` or
  `ptp valid only`.
- While waiting, `GETANCHOR: waiting N ms (ptp valid/locked/gm/mastership/
  samples)` is logged at most once per second (the trace shows only the
  first GETANCHOR of a connection).

## (parallel line) v4.1.60 (based on v4.1.59) - SETRATE / GETANCHOR (iOS 27.2)

- iOS 27.2 (iPhone, sourceVersion 1005.8.1, build 24B5089g) no longer sends
  SETRATEANCHORTIME. It sends `SETRATE {rtpTime, rate}` and then polls
  `GETANCHOR {rate}` about every 100 ms; both were answered as unknown
  methods (empty 200), the sender never got an anchor and gave up after ~8 s
  (FLUSHBUFFERED + TEARDOWN, no sound).
- New handlers. SETRATE rate 0 pauses (as SETRATEANCHORTIME rate 0);
  rate > 0 stores rtpTime. The receiver then places the anchor itself as soon
  as PTP is locked (grandmaster known, mastership >= 1 s): rtpTime sounds
  1000 ms later on the sender's PTP timeline. GETANCHOR replies with that
  anchor as `{rate, rtpTime, networkTimeSecs, networkTimeFrac,
  networkTimeFlags, networkTimeTimelineID}` - the SETRATEANCHORTIME key names
  and encoding; before PTP is usable it replies an empty 200 and the sender
  polls again. Not publicly documented: this is our reading of the exchange;
  the first reply per anchor is traced (`TRACE   GETANCHOR reply body`).
- Stream TEARDOWN clears the SETRATE/anchor state; SETRATE/GETANCHOR follow
  the play lock like SETRATEANCHORTIME.
- Host test: GETANCHOR reply plist decoded with Python plistlib.

## (parallel line) v4.1.59 (based on v4.1.58) - second sender while playing

- Fix: while a stream played, a second sender (e.g. iPhone taking over from
  the Apple TV) was refused - `rtsp_server: Failed to create client task`,
  the iPhone showed an error and only a power cycle helped. Internal RAM
  during a stream is too short for another 8 KiB client stack. The client
  task now falls back to a PSRAM stack when the internal one cannot be
  allocated (and deletes itself with the matching call).
- To make PSRAM stacks safe, RTSP code no longer touches flash: device name
  and volume are loaded into RAM by settings_init(); settings_get_* return
  the cache; settings_persist_volume() (at disconnect of the playing
  connection) writes NVS from its own short-lived internal-stack task and
  skips the write when the volume did not change.
- Host test T7: takeover while internal-stack task creation fails.

