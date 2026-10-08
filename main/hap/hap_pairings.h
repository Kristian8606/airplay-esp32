#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* HomeKit controller pairings (persistent, NVS).
 *
 * The Home app adds an AirPlay speaker to a home with a full (non-transient)
 * pair-setup. M5 carries the controller's pairing ID and long-term Ed25519
 * public key (LTPK); they are stored here. Later the home's other controllers
 * are added with /pair-add, removed with /pair-remove and listed with
 * /pair-list. pair-verify checks a known controller's signature against its
 * stored LTPK. */

#define HAP_PAIRING_ID_MAX   64
#define HAP_PAIRINGS_MAX     16
#define HAP_PERM_ADMIN       0x01

esp_err_t hap_pairings_init(void);

/* Number of stored controllers / whether at least one is an admin. */
size_t hap_pairings_count(void);
bool hap_pairings_has_admin(void);

/* Look up a controller; ltpk (32 bytes) and perm may be NULL. */
bool hap_pairings_find(const uint8_t *id, size_t id_len, uint8_t *ltpk, uint8_t *perm);

/* Add or update a controller. ESP_ERR_NO_MEM when the table is full;
 * ESP_ERR_INVALID_STATE when the id exists with a different LTPK. */
esp_err_t hap_pairings_add(const uint8_t *id, size_t id_len, const uint8_t ltpk[32],
                           uint8_t perm);

/* Remove a controller (ESP_ERR_NOT_FOUND if unknown). Removing the last
 * admin removes every pairing, as HAP requires. */
esp_err_t hap_pairings_remove(const uint8_t *id, size_t id_len);

/* /pair-list response body: state 2 plus every pairing (identifier, public
 * key, permissions), separated by TLV separators. Returns the length. */
size_t hap_pairings_list_tlv(uint8_t *out, size_t cap);

/* Log the table (one line per controller). */
void hap_pairings_log(const char *why);

/* Called (from the caller's task) whenever the admin presence changes. */
typedef void (*hap_pairings_changed_cb)(bool has_admin);
void hap_pairings_set_changed_cb(hap_pairings_changed_cb cb);
