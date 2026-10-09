#pragma once

#include <stdbool.h>

/**
 * Initialize mDNS and advertise AirPlay 2 services
 *
 * This publishes:
 * - _airplay._tcp service (AirPlay 2)
 * - _raop._tcp service (Remote Audio Output Protocol)
 *
 * With all required TXT records for iOS to recognize the device
 */
void mdns_airplay_init(void);

/* Re-publish the status flags (TXT flags / sf) after they changed. */
void mdns_airplay_update_flags(void);

/* Publish the AirPlay group of the current session (TXT gid / gcgl): the
 * sender's groupUUID and whether that group has a group leader. NULL gid =
 * no session (gid back to our own pairing identifier). */
void mdns_airplay_set_group(const char *gid, bool contains_leader);
