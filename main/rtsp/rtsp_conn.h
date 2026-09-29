#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hap.h"

/**
 * RTSP Connection State Management
 * Consolidates all session state for an AirPlay connection
 */

// Forward declaration
typedef struct rtsp_conn rtsp_conn_t;

/**
 * Connection state struct - consolidates all session state
 */
struct rtsp_conn {
  // HAP session for pairing/encryption
  hap_session_t *hap_session;
  bool encrypted_mode;

  // Volume control: Q15 fixed-point (0-32768)
  // 32768 = 0 dB (unity), 0 = mute
  volatile int32_t volume_q15;
  float volume_db;

  // Audio streaming state
  bool stream_active;
  bool stream_paused;
  // Shairport AP2 TEARDOWN semantics: a valid plist without a streams item
  // requests the RTSP connection itself to close after the 200 response.
  bool close_after_response;
  // Set once the first stream SETUP of this RTSP connection has
  // started PTP from a clean estimator (later stream SETUPs keep it).
  bool ptp_session_fresh;
  bool amp_session_active;
  // Shairport "principal_conn": only the connection holding the play lock
  // may touch the single global audio engine, PTP state, volume, amplifier
  // and event port. Other connections (GET /info, pairing, remote control,
  // another device probing) are served without disturbing playback.
  // Set/cleared by rtsp_server under its owner mutex.
  bool play_owner;
  int64_t pause_started_us;
  int64_t stream_type;    // 96=UDP realtime, 103=TCP buffered
  uint16_t data_port;     // UDP port for audio data (type 96)
  uint16_t control_port;  // UDP port for control (retransmit requests)
  uint16_t event_port;    // TCP port for server->client events
  uint16_t buffered_port; // TCP port for buffered audio (type 103)
  int event_socket; // TCP listener for event port

  // Sender address: realtime PTP source filter and retransmit requests
  uint32_t client_ip;           // Client IP (network byte order)
  uint16_t client_control_port; // Sender's control port (realtime NACKs)
};

/**
 * Create a new connection state
 * @return Allocated connection state, or NULL on failure
 */
rtsp_conn_t *rtsp_conn_create(void);

/**
 * Free connection state and associated resources
 */
void rtsp_conn_free(rtsp_conn_t *conn);

/**
 * Full cleanup when the connection closes: closes the event socket and resets
 * stream/port state; clears PTP and the peer list only for the play owner.
 * Audio is stopped by the caller.
 */
void rtsp_conn_cleanup(rtsp_conn_t *conn);

/**
 * Set volume in dB (converts to Q15 internally)
 * @param conn Connection state
 * @param volume_db Volume in dB (0 = max, -144 = mute)
 */
void rtsp_conn_set_volume(rtsp_conn_t *conn, float volume_db);
