# esp32-airplay2-surround

An **ESP32-S3 AirPlay 2 receiver that plays one speaker of a 5.1 / 7.1 stream**
from an Apple TV. Give each board a position (Front Left, Centre, Subwoofer,
Rear Right…) and several of them make a wireless surround system, kept in sync
over PTP like Apple's own speakers. Stereo AAC and ALAC still play normally.

> **Status: experimental.** It works with an Apple TV (Apple Music in Dolby
> Atmos → 7.1, HBO → 5.1, YouTube → stereo), but it is young: see
> [Limitations](#limitations).

## How it works

1. In `GET /info` the receiver advertises stereo **ALAC 44.1 kHz/16-bit**,
   **ALAC 48 kHz/24-bit**, **AAC-LC 44.1/48 kHz**, and, in multichannel builds,
   **AAC-LC 48 kHz 5.1/7.1** (`supportedFormats`, as Shairport Sync does).
   For surround content the Apple TV sends a multichannel stream to each receiver.
2. All AAC, including mono/stereo in builds with surround disabled, is decoded
   with **Fraunhofer FDK AAC** to signed32 PCM and directly normalized to
   float32. For surround, a small FDK patch synthesizes only the speaker this
   board plays and outputs just that channel, which keeps decoding at roughly
   40 % of one core instead of over
   100 %.
3. The chosen speaker goes to **both I2S slots** (L = R). EQ, loudness and the
   output filters work as usual, so a board can also drive a subwoofer with a
   low-pass filter.
4. **I2S follows the actual packet format**: 44.1 or 48 kHz for stereo,
   48 kHz for surround, and 44.1 kHz for realtime ALAC. Buffered streams can
   change codec, channels, depth and rate by SSRC without another SETUP.

The rest is the base receiver: AirPlay 2 pairing, buffered (type 103) and
realtime (type 96) streams, PTP timing with a live-tuned I2S clock, a web UI
with EQ, loudness, latency and OTA.

### Input formats

| Transport | Codec | Rate | Input channels | Frames/packet |
|---|---|---|---|---|
| Realtime UDP (96) | ALAC 16-bit | 44.1 kHz | 2 | 352 |
| Buffered TCP (103) | ALAC 16-bit | 44.1 kHz | 2 | 352 |
| Buffered TCP (103) | ALAC 24-bit | 48 kHz | 2 | 352 |
| Buffered TCP (103) | AAC-LC | 44.1 / 48 kHz | 2 | 1024 |
| Buffered TCP (103), multichannel build | AAC-LC | 48 kHz | 6 / 8 | 1024 |

ALAC 24-bit samples go directly to the float PCM pipeline without a 16-bit
intermediate conversion. AAC uses FDK's signed32 output before 16-bit
quantization, normalized by 2^31 directly into the same float32 pipeline for
stereo, selected surround speakers and mono mixes. The FDK PCM limiter is
disabled as required by this output API; DRC policy and gain are unchanged.
FDK uses noise-substitution concealment without its default extra frame of
interpolation delay, so each decoded block keeps its packet's RTP alignment.
Float32 retains its normal precision (it is not lossless signed32 storage).
I2S uses 32-bit data in 32-bit slots throughout. AirPlay's **F24 AAC** metadata
describes the announced format profile, not a lossless source bit depth.
Stereo builds advertise and play all four buffered stereo formats, but do
not advertise surround unless multichannel support is enabled.

SETUP's `audioFormat` supplies omitted rate/depth/frame/channel information;
conflicting fields are rejected, except AAC `ss=16/24/32` compatibility hints
(canonical F24 remains 24); ALAC depth remains strict. Actual buffered packet SSRC is
authoritative afterwards. Codec/channel changes at the same rate preserve
queued PCM. A rate change waits for its presentation boundary before retiring
old-rate PCM and re-priming I2S. An established RTP/PTP map is interpreted at
the active rate up to the transition packet, then rebased there to keep time
continuous. Anchor refreshes/retransmissions do not change that policy. A new
timeline after FLUSH, or the first SSRC, uses the sender anchor at the actual
packet rate; anchor revisions alone cannot identify an encoding change.
Format changes are authenticated before touching the pipeline. This is not a
gapless resampler. Only AAC blocks are muted after startup or a timestamp
discontinuity, not ALAC.

## Choosing the speaker

**In the browser** (recommended): open `http://<board-ip>/eq`, card
**"Surround speaker (5.1 / 7.1)"**. The choice applies from the next decoded
frame, without a restart, and is saved. The card also shows what is playing
right now.

**At build time** (default before anything is chosen in the browser):
`idf.py menuconfig` → **Multichannel Experiment** → *Default speaker this
receiver plays*.

| Speaker | Where | Notes |
|---|---|---|
| Front Left / Right (L / R) | ~30° left/right of the screen | main pair |
| Centre (C) | under/over the screen | most of the dialogue |
| Subwoofer (LFE) | anywhere low | only the low-frequency effects channel, not the bass of the other speakers |
| Side Left / Right (Ls / Rs) | beside the listener, ~90–110° | the surround pair of 5.1 |
| Rear Left / Right (Lrs / Rrs) | behind the listener, ~135–150° | 7.1 only; with 5.1 they play Side L/R |
| Mono mix | — | all channels except LFE; full decode, most CPU |

The speaker is found by **name**, from the channel labels of each stream, so
one setting works for 5.1 and 7.1.

**7.1 naming caveat.** Apple TV sends 7.1 as AAC channel configuration 7
(centre, three pairs, LFE). The names above follow Apple's 7.1 order (first
pair L/R, second pair sides, backs rear). The ISO reading of configuration 7
calls the first pair "front centre" and the second L/R instead. If a 7.1
speaker test plays Front where you expect Side, that is why; please open an
issue.

The EQ page's old *Source mode* (stereo / mono / left / right) does not matter
for a surround stream: both inputs carry the same speaker, so every mode gives
the same signal.

## Hardware

- **ESP32-S3** with **8 MB flash** and **8 MB octal PSRAM** (the buffered
  stream alone keeps a 6 MB compressed buffer in PSRAM).
- An I2S DAC / amplifier. Defaults: BCLK GPIO 5, LRCK/WS GPIO 6, DATA GPIO 4,
  MCLK unused; change them in `menuconfig` → **I2S Playout**.
- Optional: WS2812 VU LED (GPIO 38 by default), amplifier power-control GPIO,
  playback buttons.

## Build and flash

Built with **ESP-IDF v6.0.2**.

```bash
idf.py set-target esp32s3
idf.py menuconfig    # optional: I2S pins, default speaker, logs
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

First start: join the Wi-Fi access point **ESP32-AirPlay-Setup** and open
`http://192.168.4.1/` to enter your network. Afterwards the board shows up as
an AirPlay 2 speaker; add all of them to the Apple TV's audio output.

To build the plain stereo receiver from this tree, turn off
**Multichannel Experiment → Receive 5.1/7.1 AAC and play one channel**, and
remove the two "Multichannel experiment" lines at the end of
`sdkconfig.defaults` (they move Wi-Fi and LwIP to Core 1).

### Where the work runs

| Core 0 | Core 1 |
|---|---|
| FDK AAC decoding, RTSP, PTP | Wi-Fi driver, LwIP, AAC TCP reader, I2S playout, status / LED / log tasks |

In both build modes the decode task's 64 KiB stack and AAC signed32 scratch
buffer (64 KiB) live in PSRAM; FDK keeps ~45 KiB on the stack. Scratch allocation
can fall back to the regular heap if PSRAM allocation fails.

## Limitations

- **Experimental**, tested with one Apple TV. An iPhone sends stereo to this
  receiver; multichannel was only seen from the Apple TV.
- **Rear channels are often nearly silent in music**; they come alive with
  films and Atmos mixes.
- The **7.1 naming** (Apple vs ISO order) has not been confirmed with a channel
  test signal yet (see above).
- **CPU**: decoding one speaker of 7.1/5.1 takes ~35–50 % of Core 0
  (parsing all channels is the largest part and cannot be skipped). Mono mix
  decodes everything and may not keep up.
- **Realtime ALAC** plays at 44.1 kHz through the rate switch, but it has not
  been tested much in multichannel builds yet.
- Each switch between 44.1 and 48 kHz has a short silence while I2S is
  reconfigured.
- Some video apps take 20–30 s between the start of the AirPlay session and
  the first audio (seen with HBO); that is the sender, not a fault here.
- Wi-Fi and LwIP pinned to Core 1 (`sdkconfig.defaults`) apply to the whole
  branch, multichannel on or off.

## Logs

Useful lines in the serial or web log (`http://<board-ip>/logs`):

| Line | Meaning |
|---|---|
| `W ... NOW PLAYING \| AAC-LC \| 48000 Hz \| 24-bit (F24) \| 7.1 (8ch) \| Front Right (R)` | actual surround selection, logged when the decoder applies it |
| `W ... NOW PLAYING \| ALAC \| 48000 Hz \| 24-bit \| stereo L/R` | stereo format after successful PCM publication |
| `SETUP audioFormat=0x10000000000 -> AAC-LC 48000 Hz / F24 / 7.1` | the initial format the sender chose; F24 is AAC profile metadata |
| `AUDIO FORMAT ... -> ALAC 48000 Hz / 24-bit / stereo; SSRC ...` | actual input change, old/new SSRC and sequence |
| `AUDIO PIPELINE AAC/48000 Hz/F24/8 ch/1024 frames -> ALAC/48000 Hz/S24/2 ch/352 frames` | old/new codec, rate, AirPlay profile or ALAC integer depth, channels and block size |
| `I2S RATE 44100 -> 48000 Hz` | the physical I2S clock has changed successfully |
| `AUDIO RATE ANCHOR 44100 -> 48000 Hz, rebased at RTP=...` | timing continuity at the rate-change boundary |
| `MC: decoded channel N = back #0 -> Rear Left (Lrs)` | channel layout of the stream |
| `MC CPU … avg …% of one core` and `MC CPU stages …` | decode time per frame, split into parse / synthesis / output |
| `audio_status: … pcm=…ms` | decoded audio queued for I2S; should stay near 850 ms |

For RTSP-level debugging enable **AirPlay Logs and Diagnostics → Log every
RTSP request and response** (verbose; turn it off afterwards).

This single checkbox (`CONFIG_AIRPLAY_LOG_RTSP_REQUESTS`) controls all
`RTSP REQ` / `RTSP RSP` lines, including `/feedback`, `/command`, metadata and
progress requests. It is disabled by default. `NOW PLAYING` warnings remain
visible with both this option and **Runtime event logs** off; they are emitted
on stream start, input format changes or applied surround speaker changes,
not for every packet. The periodic status also includes codec and rate, with
AAC's AirPlay profile as `24-bit (F24)`, or ALAC's actual 16/24-bit depth.
Internal decoder/DSP/I2S details are omitted from playback and status lines.
AAC's profile does not establish the source recording's bit depth or make
the lossy codec lossless.
Each playback warning is framed by `================================================`
on separate warning-level log lines, so the complete format and speaker remain
easy to find in both serial and web logs.

For hardware regression testing, try realtime screen/system audio, buffered
44.1/48 kHz stereo AAC, lossless 48 kHz ALAC, and 5.1/7.1 speaker selection.
Then change tracks/apps and AAC/lossless output without disconnecting AirPlay,
including 44.1 → 48 → 44.1 kHz, pause/resume and seek. Check both `AUDIO FORMAT`
and `I2S RATE` logs; a new SETUP is not required for an SSRC format change.

## Licenses

- **Fraunhofer FDK AAC** (`components/fdk_aac/fdk-aac`) is under the
  Fraunhofer FDK license, [`NOTICE`](components/fdk_aac/fdk-aac/NOTICE). The
  copy here is a **Third-Party Modified Version of the Fraunhofer FDK AAC
  Codec Library for Android**; the changes and their date are listed in
  [`components/fdk_aac/MODIFICATIONS.md`](components/fdk_aac/MODIFICATIONS.md).
  The license allows redistribution in source and binary form without
  copyright fees, but **grants no patent rights**: AAC patent licenses are
  available through Via Licensing or the patent owners.
- The **rest of this repository** has no license file yet, so no license is
  granted for it beyond viewing it on GitHub.
- Other components (ESP-IDF, `esp_audio_codec`, libsodium, mDNS, cJSON,
  led_strip) come through the ESP-IDF component manager under their own
  licenses.

## Credits

- Based on [airplay-esp32](https://github.com/Kristian8606/airplay-esp32),
  the stereo AirPlay 2 receiver for the ESP32-S3.
- [Shairport Sync](https://github.com/mikebrady/shairport-sync) as the
  reference for AirPlay 2 behaviour, including the multichannel
  `supportedFormats` advertisement.
- [fdk-aac](https://github.com/mstorsjo/fdk-aac) for the standalone build of
  the Fraunhofer FDK AAC codec.

AirPlay, Apple TV and HomePod are trademarks of Apple Inc. This project is not
affiliated with or endorsed by Apple.
