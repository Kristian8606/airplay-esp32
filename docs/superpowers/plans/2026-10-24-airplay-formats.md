# AirPlay Format Coverage Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Play every Shairport Sync input format supported by this build, including 48 kHz stereo and 24-bit ALAC, with clear format-change logs.

**Architecture:** One format registry drives advertisement, SETUP validation and SSRC decoding. Buffered packets select AAC or ALAC from SSRC. Keep the current float PCM/I2S pipeline and realtime ALAC path; enable rate switching in stereo builds too.

**Tech Stack:** ESP-IDF, C, Espressif ALAC/AAC, existing FDK multichannel decoder.

**Spec:** User-approved scope in this chat, including 24-bit preservation and logs for format/rate changes.

## Global Constraints
- Work in the existing experimental checkout so the user can test it directly; no branches, commits or flashing.
- Preserve realtime ALAC 44.1 kHz/16-bit/352 and existing surround speaker selection.
- Buffered ALAC uses 352 frames; AAC uses 1024 frames.
- Advertise ALAC 44.1/16, ALAC 48/24, AAC 44.1 and 48 stereo; surround remains conditional on the existing FDK build option.
- No repository test framework exists: use temporary host regression harnesses rather than adding a new framework.

## Review Focus
- Missing SETUP rate/depth/frame keys: infer from audioFormat, reject explicit contradictions.
- Unknown or unadvertised formats: reject SETUP and never decode arbitrary SSRC values.
- Same TCP connection changes codec/rate: recreate the right decoder and never play old PCM at the new rate.
- ALAC 24-bit signed extrema/low bits and malformed decoder output: preserve precision and enforce byte bounds.
- Rate-change concurrency with FLUSH/anchors: preserve control ownership and generation cancellation.

### Task 1: ALAC PCM precision
**Files:** main/audio/alac_decoder.c, main/audio/alac_decoder.h
**Interfaces:** Add alac_decoder_decode_float with the same arguments as alac_decoder_decode, but float output; preserve the existing int16 interface for realtime.
- [x] Reproduce the missing float API with a temporary host harness.
- [x] Decode packed 16/24-bit little-endian PCM to float without 16-bit truncation; validate decoder metadata and byte lengths.
- [x] Run signed-extrema, low-bit, bounds and realtime-interface regressions.

### Task 2: Registry, advertisement and SETUP
**Files:** main/airplay_audio_formats.h, main/airplay_identity.h, main/rtsp/rtsp_handlers.c
**Interfaces:** A shared descriptor contains audioFormat, SSRC, codec, rate, channels, source bit depth, block size and name; lookup by audioFormat/SSRC and validate stream support.
- [x] Capture failing advertisement and SETUP cases with a temporary host harness.
- [x] Implement shared mappings, all supported advertisement bits and strict SETUP inference/validation.
- [x] Exercise both build variants, omitted keys, unknown formats and contradictory fields.

### Task 3: Buffered decoding and output transitions
**Files:** main/audio/audio_receiver.c, main/audio/audio_receiver.h, README.md
**Interfaces:** Consume the registry and float ALAC API; keep pipeline output stereo and update input format/rate from SSRC.
- [x] Reproduce skipped stereo48/ALAC and SETUP-rate versus SSRC mismatch.
- [x] Complete AAC/ALAC dispatch; mute discontinuities only for AAC; update rate-sensitive diagnostics.
- [x] On rate transitions, cancel old PCM generations and rebase timing at the packet boundary; synchronize playout rate before playing new-rate PCM.
- [x] Log old/new input format, SSRC, rate, depth, channels, and actual I2S rate changes.
- [x] Update format matrix and hardware testing steps in README.
- [x] Build multichannel and stereo configurations, run host regressions and diff checks, and obtain fresh-context review.

## Execution Ledger
- Scope approved by user; execution in place on exp/multichannel-advertise.
- Pre-flight: Tasks 1 and 2 are independent; Task 3 consumes both interfaces above.
- Ruling: keep the existing surround build flag rather than advertising surround in a build without FDK support.
- Hardware playback remains the user's final validation; do not claim live playback verified from host/build checks.
- Task 1: complete. Baseline ALAC harness: 66 failures; implementation: 222 checks passed with UBSan. SDK S3 disassembly confirms packed 24-bit little-endian output and first-success-only metadata.
- Task 2: complete. Advertisement harness failed on missing stereo bits, then passed in both configurations. Real bplist builder masks verified through plistlib; output bounds checked for capacities 0..1199 and 4096.
- Ruling: fix adjacent advertisement/parsing defects: remove unsupported PCM/AAC-ELD mDNS codec claims; use actual stream rate in feedback; accept integral real sr; reject truncated SETUP summaries. Parser harness: 3 failures before fixes, all 9 cases pass afterwards.
- Review: three timing/authentication findings accepted. Rate transitions must preserve old-rate queued audio until the boundary, rebase old anchor maps, and authenticate SSRC changes before mutating state.
- Ruling: add non-consuming FIFO head copying for transition authentication, caching only by validated cursor serial/stream epoch; FLUSH continues to classify the same retained head. Copy/consume/bounds/stale-token tests passed at every circular wrap position (32 cases).
- Ruling: sender anchor revision is concurrency metadata, not format metadata. For an established play sequence, retain the active-rate mapping through the SSRC transition packet and rebase there, regardless of equivalent anchor refreshes. First SSRC/new PCM timeline uses the actual packet rate. There is no attempt to infer a future anchor's sample rate from its revision.
- Ruling: keep the sender's last anchor tuple separate from the internally rebased map. Identical active-timeline anchor retransmissions must not undo a rate rebase; new anchors and identical anchors after FLUSH still commit normally. Extracted-setter regression reproduced the fault before the change and passes after it.
- Ruling: format selection validates the processor's observed PCM generation while holding control/publication locks. A FLUSH/anchor/resume race retries classification before interpreting old sequence state against the new anchor.
- Task 3: complete. Extracted actual processor/control/timing functions: baseline 39/42 failures (stereo/multichannel), final 211/216 checks pass under UBSan. A preserved pre-generation-guard source failed all three added race assertions in each build; final source passes. Transport, crypto, codecs and scheduling are deterministic stubs in this harness, not hardware/network verification.
- Final verification: ALAC 222 checks; parser 9 cases; source-anchor setter; real binary GET /info and feedback serialization in both build modes; FIFO copy/read at all 32 wrap positions; registry/SETUP in both modes; CRLF-aware diff check all pass.
- Final builds: idf.py build and idf.py -B build_nomc -D SDKCONFIG=build_nomc/sdkconfig build both succeed. Multichannel binary 0x2033d0 bytes (33% app partition free), stereo 0x169b20 bytes (53% free).
- Fresh-context review: authentication, transition timing, anchor retransmission and FLUSH-generation race findings resolved and rechecked. No additional implementation defects reported. Changes remain uncommitted in the existing checkout; no flashing performed.
