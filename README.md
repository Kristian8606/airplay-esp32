# ESP32 AirPlay 2 Receiver

AirPlay 2 speaker firmware for the **ESP32-S3**. It shows up on iPhone, iPad, Mac and Apple TV as an AirPlay speaker and plays in sync with HomePods, AirPort Express and other AirPlay 2 speakers in multiroom groups. You can add it to the **Apple Home** app as an AirPlay speaker. The output is an I2S DAC (or a DSP/amplifier with an I2S input).

Built on ESP-IDF, written in C, no Linux and no external server.

---

## Features

- **AirPlay 2 audio**:
  - realtime ALAC over UDP;
  - buffered AAC over TCP (what Apple Music and most apps use).
- **Multiroom sync** through PTP (IEEE 1588). The I2S clock is fine-tuned in ppm to follow the sender's clock, so it plays together with HomePod, AirPort Express and Apple TV.
- **Apple Home (HomeKit)**:
  - can be added to a home as an AirPlay speaker;
  - works with the home hub (Apple TV / HomePod);
  - shows artwork and playback controls in Apple TV groups.
- **Several senders**. A new sender takes over the speaker the way it does on an AirPort Express. Connections from the home hub never interrupt the music.
- **24-bit output**, 32-bit I2S slots, MCLK output.
- **Parametric EQ**:
  - up to 24 biquad filters per channel (peak, low/high pass, band pass, notch, low/high shelf);
  - per-channel preamp;
  - stereo, mono, left-only or right-only output.
- **Loudness compensation** (ISO 226): bass and a little treble are added back as the volume goes down, with a live curve in the web UI.
- **Output latency compensation** for DACs and DSPs. The value can be set by hand or measured with a wire loop back into an ADC pin.
- **Web UI**:
  - Wi-Fi setup;
  - device name;
  - firmware update (OTA);
  - EQ;
  - live logs;
  - network speed test;
  - latency.
- **Wi-Fi**:
  - captive-portal setup;
  - up to 8 saved networks;
  - optional 5 GHz preference;
  - the setup AP comes back automatically if the router is lost.
- **Optional hardware**:
  - amplifier enable pin with a delayed power-off;
  - WS2812 status/VU LED;
  - GPIO buttons (play/pause, next, previous, volume).
- **Volume** is remembered across sessions and restarts.

---

## Hardware

**Tested board:** ESP32-S3 **N8R8** (8 MB quad flash, 8 MB octal PSRAM). PSRAM is required.

**DAC:** any I2S DAC, for example a PCM5102A, or a DSP or amplifier with an I2S input.

### Default wiring

| Signal | ESP32-S3 GPIO | Notes |
|---|---|---|
| I2S MCLK | 8 | Leave unconnected if the DAC makes its own (PCM5102A: SCK to GND) |
| I2S BCLK | 11 | |
| I2S LRCK / WS | 13 | |
| I2S DATA OUT | 12 | To the DAC's DIN |
| Amplifier enable | 21 | Optional, active HIGH by default |
| WS2812 LED | 38 | Optional |
| Latency loopback ADC | 4 | Optional, see below |
| Buttons | none | Optional, set in menuconfig, each button between the GPIO and GND |

All pins can be changed in `idf.py menuconfig`. Avoid the flash/PSRAM pins, the USB/console pins and strapping pins.

### Latency loopback (optional)

To measure the real delay of the DAC/DSP chain:
1. Connect the line output (one channel) through a coupling capacitor to a divider that biases the ADC pin to about half of 3.3 V.
2. Connect that point to GPIO 4.
3. In the web UI press **Measure**. The ESP plays short chirps, finds them in the ADC signal and saves the measured latency.

---

## Building and flashing

Requirements:
- **ESP-IDF 6.0.2 or newer**.
- These components, which are downloaded automatically: `mdns`, `libsodium`, `esp_audio_codec`, `led_strip`, `cjson`.

```bash
git clone https://github.com/Kristian8606/airplay-esp32.git
cd airplay-esp32
idf.py set-target esp32s3
idf.py menuconfig          # optional: pins, HomeKit, LED, amplifier ...
idf.py build flash monitor
```

- **Version.** The firmware version is in the `VERSION` file (for example `v4.2.6`). The image is named after it: `build/airplay-esp32_vX.Y.Z.bin`. After editing `VERSION` in an existing build folder, run `idf.py reconfigure` once.
- **Web UI files.** The pages in `data/www` are built into the firmware, so there is no separate filesystem to upload.
- **Partitions.** There are two OTA slots of 3 MB each. A web UI update writes into the slot that is not running and switches to it only after the whole image has been checked.

---

## First setup

1. After flashing, the ESP opens the Wi-Fi network **`ESP32-AirPlay-Setup`** (open by default).
2. Join it with a phone. The setup page opens by itself; if it doesn't, go to **http://192.168.4.1/**.
3. Choose your Wi-Fi network, enter the password and, if you like, a device name. The default name is "ESP32 AirPlay".
4. The ESP connects to your network and turns the setup network off. The web UI is now at the ESP's address in your network, or at `http://<name>.local/`.

**Saved networks.** Up to 8 networks can be saved. At boot the ESP scans and joins the strongest saved one.

**Lost router.** If the ESP loses the router, it keeps retrying the same access point. After 5 failed attempts it also opens the setup network again, so you can always reach it.

---

## Using it

### AirPlay

The speaker appears in the AirPlay menu of iPhone, iPad, Mac, Apple TV and in Control Center under the device name. Choose it alone or together with other AirPlay 2 speakers for multiroom.

### Adding it to Apple Home

1. In the **Home** app: **+ → Add Accessory → More options…**. The ESP is listed as an AirPlay speaker.
2. Choose it. If a code is asked for, it is **3939**. If that fails, see the hint under "Known limitations".
3. Pick a room. The speaker now belongs to the home:
   - you can play to it from the Home app and with Siri;
   - it can be part of an Apple TV or HomePod group, with artwork and controls on the Apple TV;
   - anyone on the home network can still use it. Access control is "everyone".
4. To remove it, delete the accessory in the Home app. When the last admin controller is removed, the ESP drops all stored controllers and is no longer in a home.

**The home hub.** It (Apple TV or HomePod) regularly opens short connections to the speaker to check and manage it. These connections never stop or flush music that is playing.

### Several senders

If something is playing and another device starts playing to the speaker, the new device takes over at once. This is how an AirPort Express and a HomePod behave.

---

## Web UI

| Page | What it does |
|---|---|
| `/` | Status, Wi-Fi networks, device name, output latency (manual and measured), firmware update, restart |
| `/eq` | Parametric EQ editor (changes are heard live; saving stores them in flash) and loudness compensation |
| `/logs` | Live log over WebSocket (`/ws/logs`); the serial log keeps working |
| `/speedtest` | Ping, download and upload test between the browser and the ESP |

If `AIRPLAY_WEB_ADMIN_PASSWORD` is set, these actions ask for HTTP Basic login (user `admin`):
- firmware update and restart;
- Wi-Fi and name changes;
- the latency test.

### HTTP API

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/system/info` | Version, IP, MAC, Wi-Fi details, free memory, uptime, reset reason |
| POST | `/api/system/restart` | Restart |
| POST | `/api/ota/update` | Upload a firmware image (`build/airplay-esp32_vX.Y.Z.bin`) |
| GET | `/api/wifi/scan` | Scan Wi-Fi networks |
| POST | `/api/wifi/config` | Save a Wi-Fi network |
| POST | `/api/device/name` | Set the device name (AirPlay name and hostname) |
| GET / POST | `/api/eq` | Read or write the EQ settings |
| POST | `/api/audio/mute` | Mute or unmute the left or right output channel |
| GET / POST | `/api/audio/remote` | Remote control status, or send play/pause/next/previous/volume to the sender |
| GET / POST | `/api/audio/latency` | Read or set the output latency |
| POST | `/api/audio/latency/measure` | Start the loopback measurement |
| GET / POST | `/api/audio/loudness` | Loudness on/off and reference level; GET also returns the current output level and correction |

---

## Configuration (`idf.py menuconfig`)

### AirPlay Receiver

| Option | Default | Meaning |
|---|---|---|
| Setup AP SSID / password / channel | `ESP32-AirPlay-Setup` / empty / 1 | The setup Wi-Fi network |
| Prefer 5 GHz, minimum 5 GHz RSSI | off, −75 dBm | Prefer a 5 GHz access point when its signal is good enough |
| Advertise artwork support | off | Ask senders to send cover art to the ESP. The ESP has no display, so this is off by default. Artwork on the Apple TV works either way. |
| **HomeKit** | on | Allow adding the speaker to Apple Home |
| HomeKit model | `AirPlaySpeaker1,1` | Model shown to senders. The Home app only offers third-party AirPlay speakers for adding. |
| HomeKit manufacturer | `Espressif` | Shown in the Home app |
| HomeKit setup code | `3939` | Code for adding to a home |
| Hardware playback buttons | off | GPIOs for play/pause, next, previous, volume − and +, and debounce time |

### Amplifier Power Control

| Option | Default | Meaning |
|---|---|---|
| Amplifier control | on, GPIO 21, active HIGH | The pin is active while a sender is connected |
| Power-off delay | 300 s | How long the amplifier stays on after the last sender leaves |

### I2S Playout

| Option | Default | Meaning |
|---|---|---|
| MCLK / BCLK / LRCK / DATA pins | 8 / 11 / 13 / 12 | I2S pins |
| Slot width | 32-bit slots, 24-bit audio | 16-bit slots are also available |
| Output latency | 0 µs | The delay after the ESP: DAC filter, DSP, amplifier. Typical values are about 450 µs for a PCM5102A, and half the filter length for a linear-phase FIR (1024 taps at 48 kHz ≈ 10700 µs). The web UI and the measurement override this value. |
| Latency measurement | on, GPIO 4 | Adds the **Measure** button |
| Web UI admin password | empty | Protects updates, restart and settings |

### RGB Audio/VU LED

| Option | Default | Meaning |
|---|---|---|
| Enable, GPIO, brightness, refresh rate | on, 38, 96, 30 Hz | WS2812 LED |
| While playing | VU / color music | Also available: steady green, off |
| While paused | steady blue | Also available: off |
| Standby | off | Also available: dim green |

### Diagnostics

`Enable temporary AirPlay diagnostics` adds detailed logs by area. All of them are off by default and cost nothing when off:
- transport, buffer, flush;
- codec, sync, playout;
- lifecycle.

---

## How it works

### Discovery

The ESP advertises itself over mDNS:
- `_airplay._tcp` on port 7000, the AirPlay 2 service;
- `_raop._tcp`, the classic AirPlay service.

The TXT records carry:
- the feature bits;
- status flags: in a home, and a session active;
- the device and pairing IDs;
- the AirPlay group the speaker is currently playing in, like an AirPort Express does.

### Connections and pairing

- **RTSP server** (`main/rtsp`): port 7000, up to 4 connections at a time.
  - The connection whose last audio setup won **owns the audio**.
  - Other connections, such as the home hub, remote control sessions and pairing, are answered normally. They never touch audio, timing or the group state.
  - Requests that change playback (`FLUSH`, `FLUSHBUFFERED`, `PAUSE`, `SETRATEANCHORTIME`, `SETPEERS`, `SET_PARAMETER`) are only taken from the audio owner. Any other connection gets `200 OK` and a log line "… ignored: not the audio session".
- **Pairing** (`main/hap`):
  - **Transient pairing.** Normal AirPlay uses SRP with code 3939, so no code is needed.
  - **Full HomeKit pairing.** Adding to a home uses full pair-setup and stores the controllers in NVS. After that every session starts with pair-verify. The handlers `/pair-add`, `/pair-remove` and `/pair-list` keep the list of home controllers in sync.
  - **Encryption.** After pairing, RTSP is encrypted with ChaCha20-Poly1305.

### Audio paths

| Stream type | Transport | Codec | Used by |
|---|---|---|---|
| 96 (realtime) | UDP | ALAC | Senders and apps that stream with low delay instead of buffering |
| 103 (buffered) | TCP | AAC | Apple Music and most apps; seconds of audio buffered ahead |

**Buffered (AAC) path:**

1. **Receive.** The TCP reader stores the packets still encrypted in a FIFO in PSRAM.
2. **Peek.** The decoder task looks only at the first 14 bytes of the next packet: the length plus the RTP header with sequence number, RTP time and SSRC.
3. **Decide.** It checks the packet against the active `FLUSH` / `FLUSHBUFFERED` rules.
4. **Drop or play.** A packet that is to be dropped is skipped by moving the read position: no copy, no decryption, no decoding. This makes seeking, skipping tracks and taking over from another sender almost instant, even with several seconds buffered. Packets that stay are decrypted, decoded to PCM, put through the EQ and written into a PCM ring indexed by RTP time.

**Playout:**
- The playout task reads from the PCM ring at the moment given by the sender's anchor: RTP time ↔ PTP network time.
- The configured output latency is subtracted from that moment.

### Loudness compensation

At low volume the ear loses bass, and a little treble, faster than the midrange (ISO 226 equal-loudness contours). Two shelving filters put it back. They are fitted to ISO 226 within 0.6 dB from 31.5 Hz to 10 kHz:

| Filter | Corner | Slope | Gain |
|---|---|---|---|
| Low shelf | 130 Hz | 0.4 | 0.52 dB per dB below the reference |
| High shelf | 10.5 kHz | 1.0 | 0.15 dB per dB below the reference |

- **Where it runs.** In the playout task, right after the output volume. It follows the volume immediately; the EQ runs before the PCM buffer, about a second earlier.
- **Reference level.** Set on `/eq` with a 0–20 dB slider, or with the *Use current volume* button. No correction is applied at or above it.
- **No clipping.** The boost is always smaller than the attenuation, so the output never exceeds full scale.
- **Off or at the reference.** The audio passes bit-exact.
- **Cost.** Four float biquads per stereo frame.

### Synchronization

- `main/network/ptp_clock*.c` follows the group's PTP master clock. The sender gives its PTP peers through `SETPEERS`.
- A servo compares where the audio should be with where the I2S output actually is. It corrects the difference by fine-tuning the I2S clock rate in ppm, not by dropping or inserting samples.
- If the master clock steps (a new master, or the sender restarts), the filter restarts and playback re-anchors.

### Remote control

In the session setup the ESP offers an event channel and the sender connects to it. The ESP uses this channel to send play/pause, next, previous and volume commands back to the sender. These come from the hardware buttons or from `/api/audio/remote`.

---

## Logs

The default log level is INFO and is kept short:
- one line per RTSP request;
- sessions with the sender's name and model, for example `SETUP: session from "iPhone" (iPhone18,3)`;
- audio take-over;
- HomeKit add and remove;
- Wi-Fi events;
- a status line every 2 seconds while playing.

Frequent messages (volume changes, progress, keep-alives, refused remote-control streams) are at DEBUG. Logs are on the serial port and at `http://<esp>/logs`.

When a connection closes, the log shows its stack headroom, for example `stack headroom 1300 of 8192 B`. This makes stack problems easy to spot.

---

## Known limitations

- **Generic icon.** The Home app shows a generic AirPlay icon instead of the "Speaker" icon. That type is reserved for speakers with an Apple MFi authentication chip.
- **Room change while playing.** Changing a speaker's room in the Home app while it is playing doesn't apply until playback stops. AirPort Express behaves the same way.
- **Remote-control stream.** Senders sometimes ask for a remote-control data stream (type 130) on extra connections. The ESP refuses it, because artwork, controls and buttons all work without it. These attempts are logged only at DEBUG.
- **Wi-Fi roaming.** The ESP stays on the access point chosen at boot until it restarts.

**If adding to Home fails** with `pair-setup M3: wrong code` in the log, set the HomeKit setup code to `0000` in menuconfig and try again.

---

## Project layout

```
main/
  main.c                 startup: settings, Wi-Fi, mDNS, RTSP, web UI, LED, amplifier, buttons
  airplay_identity.*     device ID, features, status flags, versions
  settings.*             NVS settings (Wi-Fi networks, name, volume, EQ, latency, loudness)
  led.*, amp_control.*, playback_buttons.*
  rtsp/                  RTSP server, connections, handlers, encryption, events, remote control
  hap/                   HomeKit pairing: SRP, pair-setup, pair-verify, controllers
  audio/                 receivers (UDP/TCP), FIFO, decoders, EQ, loudness, playout, latency measurement
  network/               Wi-Fi, mDNS, PTP clock, web server, OTA, captive DNS, log stream
  plist/                 binary and XML plist reader/writer
data/www/                web UI pages (built into the firmware)
VERSION                  firmware version
partitions.csv           two 3 MB OTA slots
```

---

## Thanks

The protocol work relied on these open-source projects as references:
- [shairport-sync](https://github.com/mikebrady/shairport-sync), the reference AirPlay 2 receiver;
- [pyatv](https://github.com/postlund/pyatv), used for AirPlay and HomeKit protocol details and for diagnostics;
- the openairplay documentation of feature bits and status flags.
