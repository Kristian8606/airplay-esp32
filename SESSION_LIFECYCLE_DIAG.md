# Session lifecycle diagnostic

This build adds concise `RSESSION` logs without changing the AirPlay state machine.

The key question is whether Apple Music's RemoteControl-only bootstrap and subsequent audio setup are the same TCP/RTSP conversation and whether their session identifiers change.

Capture one Apple Music start from the first `RSESSION OPEN` through the final `RSESSION CLOSE`. Useful lines:

- `RSESSION OPEN` / `CLOSE`: stable `id=` per accepted RTSP TCP socket.
- `RSESSION RX`: method, CSeq, fd, play-owner and stream state.
- `RSESSION HDR`: `Session`, `X-Apple-Session-ID`, `Connection`, User-Agent, DACP and Active-Remote.
- `RSESSION PLIST` / `IDS` / `STREAM`: bplist session UUIDs and RC/audio stream identity.
- `RSESSION PLAYLOCK`: which TCP conversation becomes principal audio.
- `RSESSION TX`: type-130 stream teardown versus full connection teardown.

Do not enable the heavy raw protocol trace unless this concise correlation log leaves a specific field ambiguous.
