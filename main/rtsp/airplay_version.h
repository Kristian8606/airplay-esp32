#pragma once

/* AirPlay software identity, as a HomePod mini on software 27.2 advertises it
 * (Discovery, 2026-09-30): _airplay "srcvers"/"osvers"/"protovers"/"vv",
 * _raop "vs"/"ov"/"vv", /info and the RTSP "Server: AirTunes/<version>"
 * header. Senders select protocol features by these values. One definition
 * for every place. */
#define AIRPLAY_SOURCE_VERSION "1005.8.1"
#define AIRPLAY_OS_VERSION "27.2"
#define AIRPLAY_PROTOVERS "1.1"
#define AIRPLAY_VV_STR "1"
#define AIRPLAY_VV 1
