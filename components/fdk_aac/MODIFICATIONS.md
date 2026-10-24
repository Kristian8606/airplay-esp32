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

## No patent license

As `NOTICE` section 3 says, the FDK license grants no patent rights. AAC
patent licenses are available through Via Licensing or the patent owners;
using this code is your responsibility in that respect.
