#!/bin/bash
# Host tests for the ESP32 AirPlay receiver. Needs gcc + python3 (Linux/macOS).
#   FIFO:   real loopback TCP, FreeRTOS shims on pthreads, ASan/UBSan
#   bplist: regression corpus + crafted attacks + mutation fuzz, ASan/UBSan
#   volume: unity/silence/mute/ramp/dither statistics
#   flush:  buffered FLUSH classifier incl. untilSeq=0 (extracted, log replays)
#   rtsp:   connection model / play lock (real rtsp_server.c, loopback port 7000)
#   event:  updateInfo plist + event channel crypto against a Python sender
#           model (needs libsodium: SODIUM_DIR=<prefix> or pkg-config, and the
#           Python "cryptography" package; skipped otherwise)
set -e
cd "$(dirname "$0")"
ROOT=../..
OUT=${TMPDIR:-/tmp}/airplay_host_tests
mkdir -p "$OUT"
# audio_diag.h includes sdkconfig.h; an empty one = all diagnostics off.
# (Generated here because .gitignore excludes sdkconfig* files.)
printf '#pragma once\n' > "$OUT/sdkconfig.h"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

echo "== FIFO"
gcc -g -O2 -Wall -Wextra -Wno-unused-parameter $SAN -Ishim -I"$OUT" -I$ROOT/main/audio -I$ROOT/main \
    test_fifo.c shim/shim.c $ROOT/main/audio/ap2_buffered_fifo.c -lpthread -o "$OUT/test_fifo"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_fifo" 2>/dev/null

echo "== bplist parser"
gcc -g -O1 $SAN -I$ROOT/main/plist bplist_harness.c $ROOT/main/plist/bplist_parser.c -o "$OUT/bplist_harness"
"$OUT/bplist_harness" bplist_corpus/*.bplist > "$OUT/bplist_out.txt"
echo "corpus + crafted inputs: ok"
gcc -g -O1 $SAN -I$ROOT/main/plist bplist_fuzz_driver.c bplist_fuzz_target.c \
    $ROOT/main/plist/bplist_parser.c -o "$OUT/bplist_fuzz"
"$OUT/bplist_fuzz" ${FUZZ_ITERS:-300000} bplist_corpus/*.bplist

echo "== MediaRemote CommandInfo DATA array"
python3 - "$OUT/mr_supported_commands.bplist" <<'PYMR'
import plistlib, sys
items = [
    {"kCommandInfoCommandKey": 0, "kCommandInfoEnabledKey": True},
    {
        "kCommandInfoCommandKey": 24,
        "kCommandInfoEnabledKey": True,
        "kCommandInfoOptionsKey": {
            "kMRMediaRemoteCommandInfoCanBeControlledByScrubbingKey": True,
            "kMRMediaRemoteCommandInfoSupportsReferencePosition": False,
        },
    },
    {"kCommandInfoCommandKey": 25, "kCommandInfoEnabledKey": False},
]
payload = {
    "type": "updateMRSupportedCommands",
    "params": {
        "mrSupportedCommandsFromSender": [
            plistlib.dumps(x, fmt=plistlib.FMT_BINARY, sort_keys=False)
            for x in items
        ]
    },
}
open(sys.argv[1], "wb").write(
    plistlib.dumps(payload, fmt=plistlib.FMT_BINARY, sort_keys=False)
)
PYMR
gcc -g -O1 -Wall -Wextra $SAN -I$ROOT/main/plist test_mr_command_array.c \
    $ROOT/main/plist/bplist_parser.c -o "$OUT/test_mr_command_array"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_mr_command_array" \
    "$OUT/mr_supported_commands.bplist"

echo "== nested MediaRemote metadata"
python3 - "$OUT/now_playing_nested.bplist" <<'PYMETA'
import plistlib, sys
payload = {
    "type": "updateMRNowPlayingInfo",
    "params": {"type": "npi-text", "params": {
        "kMRMediaRemoteNowPlayingInfoTitle": "Song",
        "kMRMediaRemoteNowPlayingInfoElapsedTime": 31.879,
        "kMRMediaRemoteNowPlayingInfoDuration": 153.769,
        "kMRMediaRemoteNowPlayingInfoQueueIndex": 18,
        "kMRMediaRemoteNowPlayingInfoIsInTransition": False,
        "kMRMediaRemoteNowPlayingInfoArtworkData": b"x" * 135106,
    }, "mergePolicy": "replace"},
}
open(sys.argv[1], "wb").write(plistlib.dumps(payload, fmt=plistlib.FMT_BINARY, sort_keys=False))
PYMETA
gcc -g -O1 -Wall -Wextra $SAN -I$ROOT/main/plist test_nested_metadata.c \
    $ROOT/main/plist/bplist_parser.c -lm -o "$OUT/test_nested_metadata"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_nested_metadata" "$OUT/now_playing_nested.bplist"

echo "== streamConnections SETUP"
python3 - "$OUT/stream_connections_request.bplist" <<'PYREQ'
import plistlib, sys
request = {
    "loudnessNormalizationEnabled": True,
    "streams": [{
        "type": 103, "ct": 4, "spf": 1024,
        "supportsDynamicStreamID": True,
        "streamConnections": {
            "streamConnectionTypeRTP": {
                "streamConnectionKeyUseStreamEncryptionKey": True
            },
            "streamConnectionTypeRTCP": {"streamConnectionKeyPort": 1234},
            "streamConnectionTypeMediaDataControl": {
                "streamConnectionKeyEncryptionSeed": -3431997079003895594
            },
        },
    }]
}
open(sys.argv[1], "wb").write(plistlib.dumps(request, fmt=plistlib.FMT_BINARY, sort_keys=False))
PYREQ
gcc -g -O1 -Wall -Wextra $SAN -I$ROOT/main/plist -I$ROOT/main/audio -I$ROOT/main/hap \
    -I$ROOT/main/rtsp -Ishim -I"$OUT" test_stream_connections.c \
    $ROOT/main/plist/bplist_parser.c $ROOT/main/plist/bplist_builder.c \
    -o "$OUT/test_stream_connections"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_stream_connections" \
    "$OUT/stream_connections_request.bplist" "$OUT/stream_connections_response.bplist" \
    "$OUT/datastream_response.bplist"
python3 - "$OUT/stream_connections_response.bplist" "$OUT/datastream_response.bplist" <<'PYRESP'
import plistlib, sys
s = plistlib.load(open(sys.argv[1], "rb"))["streams"][0]
assert s["type"] == 103 and s["dataPort"] == 58911
assert s["controlPort"] == 40000 and s["audioBufferSize"] == 6291456
assert s["streamID"] == 123456789
sc = s["streamConnections"]
assert sc["streamConnectionTypeRTP"]["streamConnectionKeyPort"] == 58911
assert sc["streamConnectionTypeRTCP"]["streamConnectionKeyPort"] == 40000
assert sc["streamConnectionTypeMediaDataControl"]["streamConnectionKeyPort"] == 45555
assert sc["streamConnectionTypeMediaDataControl"]["streamConnectionKeyEncryptionSeed"] == -3431997079003895594
ds = plistlib.load(open(sys.argv[2], "rb"))["streams"][0]
assert ds == {"type": 130, "streamID": 1, "dataPort": 45678}
print("streamConnections + DataStream SETUP: ok")
PYRESP

echo "== volume/dither"
python3 extract_volume.py "$OUT/vol_under_test.c"
gcc -O2 -Wall -fsanitize=undefined test_vol.c "$OUT/vol_under_test.c" -lm -o "$OUT/test_vol"
"$OUT/test_vol"

echo "== RTSP server play lock"
gcc -g -O1 -Wall -Wextra -Wno-unused-parameter $SAN -Ishim -I"$OUT" -I$ROOT/main/rtsp -I$ROOT/main/audio \
    -I$ROOT/main/hap -I$ROOT/main/network -I$ROOT/main \
    test_rtsp_server.c shim/shim.c $ROOT/main/rtsp/rtsp_server.c -lpthread -o "$OUT/test_rtsp_server"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_rtsp_server" 2>"$OUT/rtsp_server.log"

echo "== updateInfo plist"
gcc -g -O1 -Wall -Wextra $SAN -I$ROOT/main/plist -I$ROOT/main/audio -I$ROOT/main/hap -I$ROOT/main/rtsp -Ishim -I"$OUT" \
    test_update_info.c $ROOT/main/plist/bplist_builder.c -o "$OUT/test_update_info"
"$OUT/test_update_info" "$OUT/update_info.bplist" "$OUT/update_info_big.bplist"
python3 - "$OUT/update_info.bplist" "$OUT/update_info_big.bplist" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], "rb"))
assert d["type"] == "updateInfo" and set(d) == {"type", "value"}
v = d["value"]
for k in ("deviceid", "features", "model", "pk", "name", "txtAirPlay", "audioLatencies"):
    assert k in v, k
t, i, items = v["txtAirPlay"], 0, []
while i < len(t):
    items.append(t[i + 1:i + 1 + t[i]].decode()); i += 1 + t[i]
assert items[0] == "deviceid=F0:9E:9E:0E:E2:40" and items[-1] == "acl=0", items
assert v["features"] == 0x1C340405C4A00 and v["model"] == "AudioAccessory5,1" and v["srcvers"] == "377.40.00"
big = plistlib.load(open(sys.argv[2], "rb"))["value"]["txtAirPlay"]
assert big == b"x" * 700
print("updateInfo plist: ok")
PY

echo "== GETANCHOR reply plist"
gcc -g -O1 -Wall $SAN -I$ROOT/main/plist -I$ROOT/main/rtsp -I$ROOT/main/audio -I"$OUT" -Ishim test_anchor_plist.c \
    $ROOT/main/plist/bplist_builder.c $ROOT/main/plist/bplist_parser.c -o "$OUT/test_anchor_plist"
"$OUT/test_anchor_plist" "$OUT/anchor.bplist"
python3 - "$OUT/anchor.bplist" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], "rb"))
def s64(v): return v - (1 << 64) if v >= 1 << 63 else v
assert d["rate"] == 1 and d["rtpTime"] == 3778452185 and d["networkTimeSecs"] == 16670, d
assert d["networkTimeFrac"] == s64(0xA455AE1CE0000000), d
assert d["networkTimeFlags"] == 0 and d["networkTimeTimelineID"] == s64(0xC4168F40983F0008), d
print("GETANCHOR reply plist: ok")
PY

echo "== event channel crypto"
SODIUM_CFLAGS=""; SODIUM_LIBS=""
if [ -n "$SODIUM_DIR" ]; then
  SODIUM_CFLAGS="-I$SODIUM_DIR/include"; SODIUM_LIBS="$SODIUM_DIR/lib/libsodium.a"
elif pkg-config --exists libsodium 2>/dev/null; then
  SODIUM_CFLAGS="$(pkg-config --cflags libsodium)"; SODIUM_LIBS="$(pkg-config --libs libsodium)"
fi
if [ -z "$SODIUM_LIBS" ] || ! python3 -c "import cryptography" 2>/dev/null; then
  echo "skipped (libsodium or python cryptography not available)"
else
  gcc -g -O1 -Wall -Wextra $SAN -Ishim -I"$OUT" -I$ROOT/main/hap -I$ROOT/main/rtsp -I$ROOT/main/audio \
      $SODIUM_CFLAGS test_event_channel.c $ROOT/main/hap/hap_crypto.c $ROOT/main/rtsp/rtsp_crypto.c \
      $SODIUM_LIBS -o "$OUT/test_event_channel"
  ASAN_OPTIONS=detect_leaks=0 python3 test_event_channel.py "$OUT/test_event_channel" "$OUT"
fi

echo "== FLUSH classifier"
python3 extract_flush.py "$OUT/flush_under_test.c"
gcc -g -O1 -Wall -Wextra -Wno-unused-function $SAN -Ishim -I"$OUT" test_flush.c "$OUT/flush_under_test.c" \
    shim/shim.c -lpthread -o "$OUT/test_flush"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_flush" 2>/dev/null
