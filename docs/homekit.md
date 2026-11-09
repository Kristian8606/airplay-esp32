# HomeKit

With `CONFIG_AIRPLAY_HOMEKIT` (menuconfig **AirPlay Receiver**, on by default)
the speaker can be added to Apple Home as an AirPlay speaker, shows the
artwork and controls of what it plays, and can be removed again. AirPlay
without a home works as before: access stays open (`acl=0`).

## Configuration

| Option | Default | Use |
|---|---|---|
| `AIRPLAY_HOMEKIT` | y | Pairings, `/configure`, HomeKit status flags and TXT keys |
| `AIRPLAY_HOMEKIT_MANUFACTURER` | `Espressif` | TXT `manufacturer`, `/info`, shown in Home |
| `AIRPLAY_HOMEKIT_SETUP_CODE` | `3939` | SRP password of the full pair-setup (try `0000` if the log says "wrong code") |
| `AIRPLAY_MODEL_ID` | `AirPlaySpeaker1,1` with HomeKit | Home does not offer a HomePod model under Add Accessory |
| `ENABLE_AIRPLAY_ARTWORK` | y with HomeKit | Adds artwork (1) to TXT `md` |

An existing `sdkconfig` keeps its own values: defaults apply only to options
it does not contain yet.

With HomeKit the speaker also announces `srcvers` 366.0, feature bit 16
(progress metadata), serial number (Wi-Fi MAC) and firmware version, like an
AirPort Express.

## Pairing

- **Adding:** the Home app uses either a full pair-setup or a transient
  pair-setup (code 3939) followed by `/pair-add`. While the speaker has no
  pairing, the first controller added is its owner (admin). A full
  pair-setup is refused once an admin exists.
- **Identity:** pair-setup M6 and pair-verify M2 present the pairing
  identifier `pi`, the same one `/configure`, mDNS and `/info` give.
- **Pair-verify:** a stored controller must sign with its stored key, or the
  connection is refused. Other clients keep open AirPlay access.
- **Management:** `/pair-add`, `/pair-remove`, `/pair-list` and `/pairings`
  need a verified admin. Adding the same admin with the same key again (a
  retried add) is acknowledged without granting admin rights to the
  connection.
- **Revocation:** a connection checks its controller against the store on
  every request; a removed controller's connections are closed.
- **Removal:** removing the last admin, or an admin's `/configure` turning
  HomeKit access control off, resets HomeKit after the reply: pairings and the Ed25519
  identity are erased and the speaker restarts with a new identity (see
  `web-reset.md`).

## Storage

NVS namespace `airplay`, keys `ed25519_pub`, `ed25519_sec` and `hk_pairs`
(16 records of 99 bytes, the format of earlier HomeKit firmware of this
speaker). Writes run on a task with its stack in internal RAM; a failed write
restores the previous table in RAM. Unreadable pairings stop AirPlay at
start-up and leave the web page available for a reset.

## Sessions, groups and status flags

- A normal session SETUP takes the audio session at once; the previous sender
  is stopped only after the new key and event port are ready. Playback still
  starts with SETRATEANCHORTIME. Without HomeKit the session changes hands at
  the stream SETUP, as before.
- `isRemoteControlOnly` sessions (Home hubs) get their own event port and
  never take or change the audio.
- Status flags: bit 10 (0x400) while in a home, bit 11 (0x800, relay) while a
  sender holds the session. Without bit 11 an Apple TV shows no artwork or
  controls for the speaker in a group.
- TXT `gid`/`gcgl` follow the sender's `groupUUID` and
  `groupContainsGroupLeader`; without a session `gid` is the speaker's `pi`.
- Flag, group and name changes are marked while a request is handled and
  published on mDNS after the reply, outside the RTSP lock, by one task at a
  time.

## /configure

The reply carries `pi`, the public key, the name, whether HomeKit access
control is on and access level 0. A new `Device_Name` is saved before the
reply (a failed save is refused); the host name and mDNS names follow after
it. Before the first pairing a transient connection may configure, as the
Home app does while adding.

## Artwork

`SET_PARAMETER` with an `image/*` body up to 2 MiB is read in parts and
discarded, never stored or decoded, so the next request stays framed. A
larger image gets 413. The transfer itself still uses Wi-Fi and CPU.

## AAC limiter (optional)

`AIRPLAY_AAC_FDK_LIMITER` (default off) enables the FDK PCM limiter on the
S32 output, before EQ and loudness (it does not limit peaks those add). Its
lookahead (15 ms: 720 frames at 48 kHz, 661 at 44.1 kHz) is subtracted from
the PCM RTP tags, so sync is kept. New or muted sequences reset the limiter;
its last samples are played out at the end of a stream, a forward gap, a
codec change or an approaching DMA deadline. It needs about 51 KiB more and
its CPU cost on the board is not measured. ALAC does not pass through it.

## To check on a speaker

Adding in Home, artwork and play/pause/volume in Home, a restart keeping the
pairing, removal and adding again, a second sender taking over during
playback, and long listening with artwork transfers.
