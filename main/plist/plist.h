#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * Simple plist builder for AirPlay
 * Builds XML plist format (easier to debug, iOS accepts it)
 */

typedef struct {
  char *buffer;
  size_t size;
  size_t capacity;
} plist_t;

/**
 * Initialize a plist builder
 */
void plist_init(plist_t *p, char *buffer, size_t capacity);

/**
 * Start XML plist document
 */
void plist_begin(plist_t *p);

/**
 * Start a dictionary
 */
void plist_dict_begin(plist_t *p);

/**
 * Add string to dictionary
 */
void plist_dict_string(plist_t *p, const char *key, const char *value);

/**
 * Add integer to dictionary
 */
void plist_dict_int(plist_t *p, const char *key, int64_t value);

/**
 * Add unsigned integer to dictionary
 */
void plist_dict_uint(plist_t *p, const char *key, uint64_t value);

/**
 * Add base64 data to dictionary
 */
void plist_dict_data(plist_t *p, const char *key, const uint8_t *data,
                     size_t len);

/**
 * End dictionary
 */
void plist_dict_end(plist_t *p);

/**
 * Start an array with key (inside dict)
 */
void plist_dict_array_begin(plist_t *p, const char *key);

/**
 * End array
 */
void plist_array_end(plist_t *p);

/**
 * End plist document
 * @return Total size of plist
 */
size_t plist_end(plist_t *p);

// ========================================
// Binary plist parser (for AirPlay 2 SETUP)
// ========================================

#define BPLIST_PEER_MAX_ADDRESSES 4
#define BPLIST_PEER_ADDRESS_MAX   64

/**
 * Parsed AirPlay 2 PTP peer entry.
 *
 * SETPEERS uses a top-level array of address strings. SETPEERSX uses a
 * top-level array of dictionaries whose useful timing fields are Addresses
 * and, on newer senders, ClockID. Keep this representation independent from
 * sockets so the plist parser remains portable and easy to host-test.
 */
typedef struct {
  char addresses[BPLIST_PEER_MAX_ADDRESSES][BPLIST_PEER_ADDRESS_MAX];
  size_t address_count;
  uint64_t clock_id;
  bool has_clock_id;
} bplist_peer_info_t;

/**
 * Parse an AirPlay 2 SETPEERS / SETPEERSX binary-plist body.
 *
 * @param plist Binary plist data.
 * @param plist_len Length of plist.
 * @param extended false for SETPEERS, true for SETPEERSX.
 * @param out Output peer array (may be NULL when out_capacity is 0).
 * @param out_capacity Number of entries available in out.
 * @param out_count Number of peer entries present in the plist. This can be
 *                  larger than out_capacity; only the first out_capacity are
 *                  written.
 * @return true when the top-level peer-list structure is valid.
 */
bool bplist_get_peer_list(const uint8_t *plist, size_t plist_len,
                          bool extended, bplist_peer_info_t *out,
                          size_t out_capacity, size_t *out_count);

/**
 * Find a data value by key in a binary plist
 * @param plist Binary plist data
 * @param plist_len Length of plist
 * @param key Key to search for (e.g., "ekey", "eiv")
 * @param out_data Output buffer for data value
 * @param out_capacity Capacity of output buffer
 * @param out_len Actual length of data found
 * @return true if found, false otherwise
 */
bool bplist_find_data(const uint8_t *plist, size_t plist_len, const char *key,
                      uint8_t *out_data, size_t out_capacity, size_t *out_len);

/**
 * Find a data value by key anywhere in a binary plist
 * @param plist Binary plist data
 * @param plist_len Length of plist
 * @param key Key to search for (e.g., "ekey", "eiv")
 * @param out_data Output buffer for data value
 * @param out_capacity Capacity of output buffer
 * @param out_len Actual length of data found
 * @return true if found, false otherwise
 */
bool bplist_find_data_deep(const uint8_t *plist, size_t plist_len,
                           const char *key, uint8_t *out_data,
                           size_t out_capacity, size_t *out_len);

/**
 * Read one DATA element from an ARRAY value located by key anywhere in a
 * binary plist. This is primarily useful for AirPlay 2 control structures such
 * as params.mrSupportedCommandsFromSender, whose array elements are themselves
 * serialized binary plists.
 *
 * @param item_index Zero-based index in the matching DATA array.
 * @param out_count Receives the full array element count when the key is found.
 * @return true when the requested element exists and is a DATA object.
 */
bool bplist_get_data_array_item_deep(const uint8_t *plist, size_t plist_len,
                                     const char *key, size_t item_index,
                                     uint8_t *out_data, size_t out_capacity,
                                     size_t *out_len, size_t *out_count);

/**
 * Get number of stream entries in a binary plist "streams" array
 * @param plist Binary plist data
 * @param plist_len Length of plist
 * @param count Output stream count
 * @return true if streams array found and count read
 */
bool bplist_get_streams_count(const uint8_t *plist, size_t plist_len,
                              size_t *count);

/**
 * Get stream details by index from a binary plist "streams" array
 * @param plist Binary plist data
 * @param plist_len Length of plist
 * @param index Stream index
 * @param type Stream type (e.g., 96)
 * @param ekey_len Length of ekey data if present
 * @param eiv_len Length of eiv data if present
 * @param shk_len Length of shk data if present
 * @return true if stream entry parsed
 */
bool bplist_get_stream_info(const uint8_t *plist, size_t plist_len,
                            size_t index, int64_t *type, size_t *ekey_len,
                            size_t *eiv_len, size_t *shk_len);

// Key/value summary of one stream dict entry
typedef struct {
  char key[64];
  uint8_t value_type; // See BPLIST_VALUE_*
  size_t value_len;
  int64_t int_value;
} bplist_kv_info_t;

#define BPLIST_VALUE_UNKNOWN 0
#define BPLIST_VALUE_INT     1
#define BPLIST_VALUE_DATA    2
#define BPLIST_VALUE_STRING  3
#define BPLIST_VALUE_UID     4
#define BPLIST_VALUE_ARRAY   5
#define BPLIST_VALUE_DICT    6

/**
 * Get key/value info for a stream dict
 * @param plist Binary plist data
 * @param plist_len Length of plist
 * @param index Stream index
 * @param out Output array for key/value info
 * @param out_capacity Capacity of output array
 * @param out_count Number of items written
 * @return true if stream entry parsed
 */
bool bplist_get_stream_kv_info(const uint8_t *plist, size_t plist_len,
                               size_t index, bplist_kv_info_t *out,
                               size_t out_capacity, size_t *out_count);

/**
 * Inspect the optional AirPlay 2 streamConnections dictionary for one stream.
 * Bit 59 (SupportsAudioStreamConnectionSetup) makes modern senders describe
 * the transport endpoints this way instead of relying only on the legacy
 * controlPort/dataPort fields.  The receiver must mirror the requested
 * connection types in its SETUP response and add streamConnectionKeyPort.
 *
 * @return true when a valid streamConnections dictionary was present.
 */
typedef struct {
  bool has_rtp;
  bool has_rtcp;
  bool has_media_data_control;
  bool has_media_data_control_seed;
  uint64_t media_data_control_seed;
} bplist_stream_connection_info_t;

/** Parse streamConnections types plus the optional MediaDataControl seed. */
bool bplist_get_stream_connection_info(
    const uint8_t *plist, size_t plist_len, size_t index,
    bplist_stream_connection_info_t *out);

/* Compatibility wrapper when only the connection type flags are needed. */
bool bplist_get_stream_connection_types(const uint8_t *plist,
                                        size_t plist_len, size_t index,
                                        bool *has_rtp, bool *has_rtcp,
                                        bool *has_media_data_control);

/**
 * Find stream-specific crypto fields in a binary plist
 * @param plist Binary plist data
 * @param plist_len Length of plist
 * @param stream_type Stream "type" to match (e.g., 96 for audio)
 * @param ekey Output buffer for encrypted key (optional)
 * @param ekey_capacity Capacity of ekey buffer
 * @param ekey_len Length of ekey found
 * @param eiv Output buffer for IV (optional)
 * @param eiv_capacity Capacity of eiv buffer
 * @param eiv_len Length of eiv found
 * @param shk Output buffer for shared key (optional)
 * @param shk_capacity Capacity of shk buffer
 * @param shk_len Length of shk found
 * @return true if any crypto field was found for the stream, false otherwise
 */
bool bplist_find_stream_crypto(const uint8_t *plist, size_t plist_len,
                               int64_t stream_type, uint8_t *ekey,
                               size_t ekey_capacity, size_t *ekey_len,
                               uint8_t *eiv, size_t eiv_capacity,
                               size_t *eiv_len, uint8_t *shk,
                               size_t shk_capacity, size_t *shk_len);

/**
 * Find an integer value by key in a binary plist
 * @param plist Binary plist data
 * @param plist_len Length of plist
 * @param key Key to search for
 * @param out_value Output for integer value
 * @return true if found, false otherwise
 */
bool bplist_find_int(const uint8_t *plist, size_t plist_len, const char *key,
                     int64_t *out_value);

/** Find a top-level boolean value by key in a binary plist. */
bool bplist_find_bool(const uint8_t *plist, size_t plist_len, const char *key,
                      bool *out_value);

/**
 * Find a real/float value by key in a binary plist
 * Handles both real and integer values (converting int to double)
 * @param plist Binary plist data
 * @param plist_len Length of plist
 * @param key Key to search for
 * @param out_value Output for double value
 * @return true if found, false otherwise
 */
bool bplist_find_real(const uint8_t *plist, size_t plist_len, const char *key,
                      double *out_value);

/**
 * Find a string value by key in a binary plist
 * @param plist Binary plist data
 * @param plist_len Length of plist
 * @param key Key to search for
 * @param out_str Output buffer for string value
 * @param out_capacity Capacity of output buffer
 * @return true if found, false otherwise
 */
bool bplist_find_string(const uint8_t *plist, size_t plist_len, const char *key,
                        char *out_str, size_t out_capacity);


/**
 * Deep-search variants used for nested AirPlay 2 control/metadata plists.
 * They traverse dictionaries/arrays with the same bounded visit budget as
 * bplist_find_data_deep(), so malformed self-references remain safe.
 */
bool bplist_find_int_deep(const uint8_t *plist, size_t plist_len,
                          const char *key, int64_t *out_value);
bool bplist_find_bool_deep(const uint8_t *plist, size_t plist_len,
                           const char *key, bool *out_value);
bool bplist_find_real_deep(const uint8_t *plist, size_t plist_len,
                           const char *key, double *out_value);
bool bplist_find_string_deep(const uint8_t *plist, size_t plist_len,
                             const char *key, char *out_str,
                             size_t out_capacity);
bool bplist_find_data_len_deep(const uint8_t *plist, size_t plist_len,
                               const char *key, size_t *out_len);

// ========================================
// Binary plist builders (for AirPlay SETUP responses)
// ========================================

/**
 * Build initial SETUP response bplist (no streams array)
 * Returns eventPort and timingPort.
 * @param out Output buffer
 * @param capacity Buffer capacity
 * @param event_port Event port to include in response
 * @return Length of generated bplist, or 0 on error
 */
size_t bplist_build_initial_setup(uint8_t *out, size_t capacity,
                                  uint16_t event_port);

/**
 * Build stream SETUP response bplist (with streams array)
 * Returns streams[] array with type, dataPort, controlPort and, for buffered
 * streams, audioBufferSize.
 * @param out Output buffer
 * @param capacity Buffer capacity
 * @param stream_type Stream type (96=realtime UDP, 103=buffered TCP)
 * @param data_port Data port to include
 * @param control_port Control port to include
 * @param audio_buffer_size Audio buffer size to advertise
 * @param stream_id Receiver-assigned dynamic stream ID
 * @param include_stream_id Include streamID in the stream descriptor
 * @param stream_connection_rtp Mirror streamConnectionTypeRTP with data port
 * @param stream_connection_rtcp Mirror streamConnectionTypeRTCP with control port
 * @param stream_connection_mdc Mirror MediaDataControl with port and seed
 * @param media_data_control_port Dedicated encrypted DataStream listener port
 * @param media_data_control_seed Seed echoed from the sender's MDC request
 * @return Length of generated bplist, or 0 on error
 */
size_t bplist_build_stream_setup(uint8_t *out, size_t capacity,
                                 int64_t stream_type, uint16_t data_port,
                                 uint16_t control_port,
                                 uint32_t audio_buffer_size,
                                 uint32_t stream_id, bool include_stream_id,
                                 bool stream_connection_rtp,
                                 bool stream_connection_rtcp,
                                 bool stream_connection_mdc,
                                 uint16_t media_data_control_port,
                                 uint64_t media_data_control_seed);

/** Build a type-130 dedicated DataStream SETUP response. */
size_t bplist_build_datastream_setup(uint8_t *out, size_t capacity,
                                     uint16_t data_port, uint32_t stream_id,
                                     bool include_data_port);

/**
 * Build feedback response bplist
 * Returns a streams array with type and sample rate for keepalive.
 * This response prevents iPhone from sending TEARDOWN during extended pause.
 * @param out Output buffer
 * @param capacity Buffer capacity
 * @param stream_type Stream type (103 for buffered audio)
 * @param sample_rate Sample rate (44100.0)
 * @return Length of generated bplist, or 0 on error
 */
size_t bplist_build_feedback_response(uint8_t *out, size_t capacity,
                                      int64_t stream_type, double sample_rate);

/**
 * Build /info response bplist.
 * Mirrors the XML /info response for RTSP clients that require binary plists.
 * @param out Output buffer
 * @param capacity Buffer capacity
 * @param device_id Device MAC string
 * @param device_name User-visible AirPlay device name
 * @param public_key HAP Ed25519 public key
 * @param public_key_len Public key length
 * @param features AirPlay feature bitmask
 * @param protocol_version AirPlay protocol version value ("vv")
 * @return Length of generated bplist, or 0 on error
 */
size_t bplist_build_info_response(uint8_t *out, size_t capacity,
                                  const char *device_id,
                                  const char *device_name, const char *model,
                                  const uint8_t *public_key,
                                  size_t public_key_len, uint64_t features,
                                  int64_t protocol_version);

/**
 * Build the AirPlay 2 event-channel "updateInfo" plist, as Shairport Sync
 * sends it once the sender connects to the event port:
 *   { type = "updateInfo"; value = <same dict as /info> + txtAirPlay }
 * @param txt _airplay._tcp TXT record data (length-prefixed "key=value")
 * @return Length of generated bplist, or 0 on error
 */
size_t bplist_build_update_info(uint8_t *out, size_t capacity,
                                const char *device_id, const char *device_name,
                                const char *model, const uint8_t *public_key,
                                size_t public_key_len, uint64_t features,
                                int64_t protocol_version, const uint8_t *txt,
                                size_t txt_len);

/**
 * Render a binary plist as compact text for the protocol trace, e.g.
 * {streams=[{type=103, ct=4, shk=<data 32>}], timingProtocol="PTP"}.
 * Bounded like the other parsers (depth, visit budget, spans); truncated at
 * out_capacity and always NUL-terminated.
 * @return Length written (0 if the input is not a valid bplist)
 */
size_t bplist_describe(const uint8_t *plist, size_t plist_len, char *out,
                       size_t out_capacity);

/**
 * GETANCHOR reply (receiver-placed anchor): { rate, rtpTime, networkTimeSecs,
 * networkTimeFrac, networkTimeFlags, networkTimeTimelineID } with the same
 * encoding the sender uses in SETRATEANCHORTIME (frac = 2^-64 s units,
 * timeline id = PTP clock id as a 64-bit integer).
 * @return Length of generated bplist, or 0 on error
 */
size_t bplist_build_anchor(uint8_t *out, size_t capacity, uint64_t rate,
                           uint64_t rtp_time, uint64_t network_time_secs,
                           uint64_t network_time_frac, uint64_t flags,
                           uint64_t timeline_id);
