# Web reset

The receiver main page has a Reset card with two explicitly confirmed actions.

| Action | Erased | Preserved |
|---|---|---|
| Reset HomeKit | All controller pairings and Ed25519 public/private identity in airplay NVS | Wi-Fi networks, device name, EQ/source/preamp/mute configuration, loudness, volume, output latency and unrelated NVS data |
| Factory Reset | Entire default NVS partition, including old namespaces/versions; the entire legacy storage SPIFFS partition | Installed firmware, OTA boot selection, PHY partition and NVS encryption-key partition |

A reboot follows either action. HomeKit keys are newly generated at boot; the
stable hardware MAC/pairing UUID remains. The old accessory entry on Apple's
controllers is outside the board's storage: remove it in Apple Home before
adding the speaker again. Factory reset boots with no saved Wi-Fi credentials
and exposes the setup access point. Values compiled with menuconfig are the
factory defaults, including any compiled web admin password; reset does not
change the firmware binary or eFuses.

Implementation: main/device_reset.c registers POST /api/system/reset. The
existing web admin check applies. Body is bounded to 127 bytes with exactly
two fields and no trailing JSON data. Content-Type must be application/json;
cross-site browser requests are rejected. Requests are rejected while app_main
is initializing services, to avoid racing identity creation. A failed HAP
identity/pairings initialization leaves the web server available for recovery
without starting AirPlay or audio.

Payloads:

```json
{"mode":"homekit","confirm":"RESET HOMEKIT"}
{"mode":"factory","confirm":"ERASE ALL"}
```

The existing single HTTP task (internal 8 KiB stack) temporarily runs at
priority 1. The web reset allocates no task, queue or extra stack of its own.
Removing the speaker in Apple Home (an authenticated /pair-remove of the last
administrator, or an admin's /configure turning HomeKit access control off) takes the
same HomeKit reset path after the RTSP reply: a small static task created at
start-up hands the work to the HTTP task, which stops AirPlay, erases the keys
and restarts.
web_audio_pause stops RTSP, waits for sessions and associated NVS workers to
finish, and releases audio before erasure. The HTTP handler stays occupied
until reboot, so queued settings requests cannot repopulate erased NVS. Wi-Fi
uses RAM storage and only reads the saved network list. The legacy storage
partition is unmounted and erased in <=64 KiB chunks with a scheduler yield.

Only a completed erase receives success=true. Erase failures return HTTP 500
with success=false and restarting=true, then reboot as well: partially erased
storage must not be combined with live old RAM keys. A failed audio quiesce
never starts erasure and follows the existing maintenance recovery. Unexpected
power loss during erasure is not atomic; reset can be repeated after boot.
