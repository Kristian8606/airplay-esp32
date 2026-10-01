#pragma once

/* Protocol trace for the AirPlay 2 UDP control port of a buffered stream
 * (Kconfig AIRPLAY_PROTOCOL_TRACE). Logs what the sender sends to it; with the
 * trace off both calls are no-ops. The caller keeps owning the socket:
 * ap2_control_unwatch() must be called before the socket is closed. */

/* Start logging packets that arrive on sock (replaces a previous socket). */
void ap2_control_watch(int sock, unsigned port);

/* Stop logging sock. Returns once the watcher no longer uses it. */
void ap2_control_unwatch(int sock);
