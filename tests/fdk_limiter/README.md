# S32 FDK limiter regression checks

Run `python3 tests/fdk_limiter/run_tests.py` on a host with CMake, Ninja, a C/C++
compiler and FFmpeg. The decoder libraries are built from this repository;
AAC fixtures are generated locally. The host build lives in ignored
`build-fdk-limiter-host`.

The real FDK implementation is bounds-instrumented. The real TCP receiver uses
pthread-backed FreeRTOS shims and loopback sockets with undefined-behavior
instrumentation. This checks protocol/algorithm behavior, not ESP32 CPU timing.
The on/off wrappers also publish through the actual host-buildable PCM FIFO.

`CONFIG_AIRPLAY_AAC_FDK_LIMITER=y` enables the firmware experiment. Default is off;
`sdkconfig.limiter-test` supplies the setting. Current working sdkconfig has it on.
A new AAC decoder logs `FDK PCM limiter ON: S32, lookahead ... RTP compensated`.
AAC-only: ALAC bypasses it. EQ/loudness are after it and may still overload output.
