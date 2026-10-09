#pragma once

#include <stddef.h>

/* The model below depends on menuconfig: CONFIG_ values must be visible here
 * whatever the including file included before. */
#include "sdkconfig.h"

/* How this receiver presents itself to AirPlay 2 senders. Used by mDNS
 * (_airplay/_raop TXT records), GET /info and the RTSP "Server" header, so
 * all of them always agree. */

/* AudioAccessory6,1 = HomePod (2nd generation). The Home app does not offer
 * a "HomePod" for adding as an AirPlay accessory (HomePods are set up by
 * proximity), so the HomeKit build presents a third-party speaker model. */
#if defined(CONFIG_AIRPLAY_HOMEKIT) && defined(CONFIG_AIRPLAY_HOMEKIT_MODEL)
#define AIRPLAY_MODEL          CONFIG_AIRPLAY_HOMEKIT_MODEL
#else
#define AIRPLAY_MODEL          "AudioAccessory6,1"
#endif
#ifdef CONFIG_AIRPLAY_HOMEKIT
/* As an AirPort Express (and shairport-sync) report it. */
#define AIRPLAY_SOURCE_VERSION "366.0"
#else
#define AIRPLAY_SOURCE_VERSION "377.40.00"
#endif
#define AIRPLAY_SERVER_HEADER  "AirTunes/" AIRPLAY_SOURCE_VERSION
#define AIRPLAY_PROTOVERS      "1.1"
/* "vv" / protocol version: AirPlay 2 only. */
#define AIRPLAY_PROTOCOL_VERSION 2
/* statusFlags / "flags" / "sf": 0x4 = audio receiver. Bit 10 (0x400) is
 * added while the receiver is in a HomeKit home (an admin controller is
 * paired), as an AirPort Express in a home advertises 0x404. Use
 * airplay_status_flags() for the current value. */
#define AIRPLAY_STATUS_FLAGS   0x4
#define AIRPLAY_STATUS_HOMEKIT 0x400
/* Bit 11 (0x800), DeviceSupportsRelay: set while a sender holds an audio
 * session, cleared when it ends (shairport-sync does the same around its
 * play lock; an AirPort Express in a group advertises 0xc04). Senders then
 * describe the speaker with canRelayCommunicationChannel. Bit 17 is not
 * used: it shows up as "isAirPlayReceiverSessionActive", which an AirPort
 * playing in a group does not report. */
#define AIRPLAY_STATUS_RELAY 0x800
#include <stdbool.h>
#include <stdint.h>
uint32_t airplay_status_flags(void);
/* Returns true when the flags changed (caller re-publishes them). */
bool airplay_set_session_active(bool active);

/* Feature bits advertised in mDNS "features"/"ft" and GET /info.
 * Key bits:
 *   Bit 38: SupportsCoreUtilsPairingAndEncryption
 *   Bit 46: SupportsHKPairingAndAccessControl
 *   Bit 48: SupportsTransientPairing */
#define AIRPLAY_FEATURES_HI 0x1C340
#ifdef CONFIG_AIRPLAY_HOMEKIT
/* + bit 16 (progress metadata), as an AirPort Express advertises. Its MFi
 * bit 26 is not copied: senders then demand MFi authentication
 * (/auth-setup), which needs Apple's authentication chip. */
#define AIRPLAY_FEATURES_LO 0x405D4A00
#else
#define AIRPLAY_FEATURES_LO 0x405C4A00
#endif

/* Pairing identity UUID ("pi" in mDNS and GET /info), lowercase
 * 8-4-4-4-12 form. Derived from the Wi-Fi MAC, so it is stable across
 * reboots and unique per device. */
#define AIRPLAY_PAIRING_ID_LEN 37 /* 36 chars + NUL */
void airplay_get_pairing_id(char *out, size_t len);

/* Serial number (Wi-Fi MAC in hex, 12 chars) and firmware revision (the
 * project version without the leading "v", e.g. "4.2.5"), as the Home app
 * shows them. */
void airplay_get_serial_number(char *out, size_t len);
void airplay_get_firmware_revision(char *out, size_t len);
