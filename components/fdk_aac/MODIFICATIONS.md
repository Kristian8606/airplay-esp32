# Third-Party Modified Version of the Fraunhofer FDK AAC Codec Library for Android

`fdk-aac/` holds the **decoder** libraries of fdk-aac v2.0.3
([github.com/mstorsjo/fdk-aac](https://github.com/mstorsjo/fdk-aac), tag `v2.0.3`,
commit `716f439`): libAACdec, libArithCoding, libDRCdec, libFDK, libMpegTPDec,
libPCMutils, libSACdec, libSBRdec, libSYS, plus `NOTICE` (the license) and
`ChangeLog`. The encoder libraries are not included.

The license terms are in [`fdk-aac/NOTICE`](fdk-aac/NOTICE). As it requires,
this is a modified version: it is a **Third-Party Modified Version of the
Fraunhofer FDK AAC Codec Library for Android**, not the original library.

## Changes (2026-10-24)

All changes are in `libAACdec`, each one marked in the source with the comment
`airplay-esp32 patch`, and each changed file carries a dated notice under its
header. The unmodified import is git commit `8667b02`; `git diff 8667b02 --
components/fdk_aac/fdk-aac` shows every change.

| File | Change |
|---|---|
| `include/aacdecoder_lib.h` | New API: `aacDecoder_SetSynthChannel()`, `aacDecoder_SetTimeHook()`, `aacDecoder_GetSynthInfo()`. |
| `src/aacdecoder.h` | Fields in `AAC_DECODER_INSTANCE` for the selected channel, the per-plane channel labels, the single-channel output flag and the synthesis timestamps. |
| `src/aacdecoder.cpp` | Synthesis (concealment, DRC, IMDCT, time-domain fading) runs only for the channel selected by label; the others are still parsed but their planes are neither synthesized nor cleared. Plane labels are remembered per frame. Timestamps at the start and end of synthesis. |
| `src/aacdecoder_lib.cpp` | When exactly one channel was synthesized and nothing needs the others (no SBR/MPS, limiter, DRC level normalization or USAC), skip `pcmDmx` and write only that plane, mono, scaled like the normal output path. Implementation of the new API. |

Purpose: an ESP32-S3 plays one speaker of a 5.1/7.1 AirPlay stream and cannot
synthesize all eight channels of AAC-LC in real time. Selecting nothing
(`ACT_NONE`, the default) keeps the stock behaviour.

The single-channel output was checked on a host against the full decode:
7.1 AAC from the FDK encoder decodes bit-identically through both paths for
each of the 8 channels.

## Changes (2026-10-26)

`libAACdec/include/aacdecoder_lib.h` and `src/aacdecoder_lib.cpp` add
`aacDecoder_DecodeFrame32()`. It shares the original decoder, history and control
logic, but exports signed 32-bit PCM from `PCM_DEC` before the stock `INT_PCM`
16-bit rounding. Both full interleaved output and single-channel synthesis use
the same scaling as the original output. Full output retains the existing DRC
normalization, channel mapping and saturation. Invalid output is cleared using
the appropriate sample width, and null/nonpositive output buffers are rejected.
The original S16 API remains available; `INT_PCM` and the library's other
16-bit APIs have not been redefined.

The new API requires the stock S16-output PCM limiter to be disabled and rejects
AAC-SLS output; it never silently substitutes an upshifted S16 result. The
application already disables the limiter and advertises AAC-LC. AAC is lossy:
the output width describes decoder precision, not a recoverable original PCM
bit depth. The ESP32 wrapper uses this API for stereo as well as surround.

## Changes (2026-10-26, speed-ups)

| File | Change |
|---|---|
| `libFDK/include/clz.h`, new `libFDK/include/clz_builtin.h` | On Xtensa (`__XTENSA__`, or `FDK_USE_BUILTIN_CLZ` on a host), `fixnormz_D`/`fixnorm_D` use `__builtin_clz`/`__builtin_clrsb` (NSAU/NSA on the ESP32-S3) instead of FDK's one-bit-per-iteration fallback loops. Identical results for all 2^32 inputs. |
| `libAACdec/src/channel.h`, `channel.cpp` | `CChannelElement_Read()` takes `skipSpectralDecode`; when set, the element's bits are still read but its spectrum is not inverse quantized. |
| `libAACdec/src/aacdecoder.h`, `aacdecoder.cpp`, `aacdecoder_lib.cpp` | With a channel selected by `aacDecoder_SetSynthChannel()`, elements that do not hold it skip inverse quantization and `CChannelElement_Decode()` (M/S, intensity, PNS, TNS). The element is found from the previous frame's synthesis; a new selection decodes all elements until its channel has been synthesized once. CCE parsing (dummy channel and work buffers, ~35 KiB of stack) moved from `CAacDecoder_DecodeFrame()` into a separate non-inlined function, so that stack is only used when a CCE element occurs; `CAacDecoder_DecodeFrame()` itself drops from 35.6 KiB to ~1 KiB (Xtensa `-fstack-usage`). |

Checked on a host build that emulates the ESP32 (no x86 helpers): FDK-encoded
7.1, 5.1 and stereo AAC-LC decode bit-identically to the previous version for
every speaker, with full output, and while switching speakers every 37 frames.
Instruction counts for one speaker of 7.1 dropped by about half (clz ~32 %,
element skip a further ~27-37 %); this is a host measurement, not ESP32 timing.

## No patent license

As `NOTICE` section 3 says, the FDK license grants no patent rights. AAC
patent licenses are available through Via Licensing or the patent owners;
using this code is your responsibility in that respect.
