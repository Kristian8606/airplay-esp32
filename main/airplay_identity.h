#pragma once

/* How this receiver presents itself to AirPlay 2 senders. Used by mDNS
 * (_airplay/_raop TXT records), GET /info and the RTSP "Server" header, so
 * all of them always agree. */

/* AudioAccessory6,1 = HomePod (2nd generation). */
#define AIRPLAY_MODEL          "AudioAccessory6,1"
#define AIRPLAY_SOURCE_VERSION "377.40.00"
#define AIRPLAY_SERVER_HEADER  "AirTunes/" AIRPLAY_SOURCE_VERSION
#define AIRPLAY_PROTOVERS      "1.1"
/* "vv" / protocol version: AirPlay 2 only. */
#define AIRPLAY_PROTOCOL_VERSION 2
/* statusFlags / "flags" / "sf": 0x4 = audio receiver. */
#define AIRPLAY_STATUS_FLAGS   0x4
/* Pairing identity UUID ("pi"). */
#define AIRPLAY_PAIRING_ID     "00000000-0000-0000-0000-000000000000"

/* Feature bits advertised in mDNS "features"/"ft" and GET /info.
 * Key bits:
 *   Bit 38: SupportsCoreUtilsPairingAndEncryption
 *   Bit 46: SupportsHKPairingAndAccessControl
 *   Bit 48: SupportsTransientPairing */
#define AIRPLAY_FEATURES_HI 0x1C340
#define AIRPLAY_FEATURES_LO 0x405C4A00
