#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hap.h"
#include "sdkconfig.h"

/**
 * RTSP Connection State Management
 * Consolidates all session state for an AirPlay connection
 */

// Forward declarations
typedef struct rtsp_conn rtsp_conn_t;
typedef struct rtsp_datastream rtsp_datastream_t;

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
  // SETRATE/GETANCHOR (receiver-placed anchor, iOS 27.2): SETRATE gives the
  // RTP time to start from; the receiver chooses when it sounds and reports
  // that anchor in the GETANCHOR reply.
  bool setrate_pending;      // SETRATE rate>0 received, anchor not placed
  bool anchor_placed;        // anchor below is valid for GETANCHOR replies
  uint32_t setrate_ms;       // when the pending SETRATE arrived (ms since boot)
  uint32_t anchor_wait_log_ms; // last "GETANCHOR: waiting" log (rate limit)
  bool getanchor_traced;     // trace shows one GETANCHOR poll per SETRATE
  uint32_t setrate_rtp;
  uint64_t anchor_clock_id;
  uint64_t anchor_ptp_ns;
  bool anchor_reply_traced;  // protocol trace: first reply per anchor logged
  // Shairport AP2 TEARDOWN semantics: a valid plist without a streams item
  // requests the RTSP connection itself to close after the 200 response.
  bool close_after_response;
  // Set when this RTSP connection acquires the play lock and starts a clean
  // PTP control session. Stream SETUPs must preserve samples learned after
  // SETPEERS/SETPEERSX instead of resetting the estimator again.
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
  /* Buffered AP2 still advertises a UDP control/RTCP port.  Realtime owns
   * that socket inside realtime_receiver; buffered audio has no UDP receiver,
   * so the RTSP connection keeps a bound socket alive for the stream lifetime
   * (matching Shairport Sync's separate AP2 control socket). */
  int buffered_control_socket;

  /* Dedicated encrypted AirPlay 2 DataStream channels.  MediaDataControl is
   * tied to the current audio stream; remote_control_datastream belongs to a
   * type-130 RemoteControlOnly RTSP conversation. */
  rtsp_datastream_t *media_data_control;
  rtsp_datastream_t *remote_control_datastream;
  uint16_t media_data_control_port;
  uint16_t remote_control_data_port;

  /* AirPlay 2 senders that advertise supportsDynamicStreamID expect the
   * receiver to assign a 32-bit streamID in the SETUP response. */
  uint32_t stream_id;

  // State accepted from the modern LOUDNESSNORMALIZATION method.  The
  // transport/state is implemented; DSP loudness normalization is not.
  bool loudness_normalization_enabled;

  // Sender address: realtime PTP source filter and retransmit requests
  uint32_t client_ip;           // Client IP (network byte order)
  uint16_t client_control_port; // Sender's control port (realtime NACKs)

#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE
  // Method+path hashes already traced on this connection.
  uint32_t trace_seen[32];
  uint8_t trace_seen_count;
#endif
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
