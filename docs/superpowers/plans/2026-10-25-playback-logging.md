# Playback Logging Implementation Plan

**Goal:** Implement the approved warning showing actual input codec, rate, layout and selected surround speaker, and remove verbose RTSP request/response logs.

**Architecture:** Shared warning formatting for stereo/realtime and surround; emit only on successful stream start, format switch or applied speaker change. Preserve the audio pipeline and existing real-time I2S ownership.

## Tasks
- [x] Verify missing full playback warning with a temporary host regression.
- [x] Add warning formatting and integrate into buffered publication, realtime decoding and actual multichannel speaker application.
- [x] Replace the hardcoded buffered AAC status with actual codec/rate/PCM depth.
- [x] Disable AIRPLAY_LOG_RTSP_REQUESTS in current sdkconfig and persistent defaults; leave errors, playback warnings and status enabled.
- [x] Update logging documentation, run host checks and build, and review the focused changes without committing or flashing.

## Constraints
- No new test framework, runtime dependencies or per-packet logging.
- Stereo must explicitly say surround selection is inactive; multichannel identifies the actual selected speaker or mono fallback, not just the configured preference.
- Input channels and intermediate PCM precision must not be confused with I2S slot width.
- All RTSP REQ/RSP examples share one compile-time checkbox; do not disable general logging.

## Verification
- The actual mc_apply_speaker extraction failed the NOW PLAYING assertion before the change, then passed surround 5.1/7.1, actual speaker/channel, unchanged-selection suppression, mono fallback and stereo tests using the real shared formatter.
- Existing receiver behavior checks pass: 211 stereo / 216 multichannel. ALAC precision checks: 222 passed. Anchor, parser and FIFO regressions pass; CRLF-aware diff check passes.
- Fresh-context focused review reported no issues.
- ESP-IDF build succeeds; application image is 0x203460 bytes, 33% of the OTA slot remains free. Initial CMake regeneration hit sandbox sysctl restrictions and succeeded with approved escalation.
- No commits or hardware flashing; configuration changes take effect on the device after installing the newly built firmware.
