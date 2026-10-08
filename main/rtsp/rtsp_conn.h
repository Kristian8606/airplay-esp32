#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hap.h"
#include "audio_receiver.h"

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
  /* One serialized client task owns read/write crypto scratch. */
  uint8_t *crypto_scratch;

  // AirPlay volume in dB (0 = max, -144 = mute); the Q15 gain lives in
  // audio_receiver (audio_receiver_set_volume_q15).
  float volume_db;

  // Audio streaming state
  bool stream_active;
  bool stream_paused;
  // Shairport AP2 TEARDOWN semantics: a valid plist without a streams item
  // requests the RTSP connection itself to close after the 200 response.
  bool close_after_response;
  // Set once the first stream SETUP of this RTSP connection has started PTP
  // from a clean estimator (later stream SETUPs keep it).
  bool ptp_session_fresh;
  bool amp_session_active;
  int64_t pause_started_us;
  /* Last committed AP2 setup; only the RTSP owner changes this snapshot. */
  bool setup_format_valid;
  audio_format_t setup_format;
  audio_encrypt_t setup_encryption;
  uint32_t setup_latency;
  int64_t stream_type;    // 96=UDP realtime, 103=TCP buffered
  uint16_t data_port;     // UDP port for audio data (type 96)
  uint16_t control_port;  // UDP port for control (retransmit requests, type 96)
  uint16_t event_port;    // TCP port for server->client events
  uint16_t buffered_port; // TCP port for buffered audio (type 103)
  int event_socket;       // TCP listener for event port (until handed over)

  /* Remote-control-only connection (initial SETUP with isRemoteControlOnly):
   * never owns audio, PTP or the audio event channel. */
  bool rc_only;
  /* This connection did an audio SETUP and is the single owner of the global
   * audio / PTP / event-channel state. Only the owner resets them on close. */
  bool owns_audio;
  /* Remote control (type 130) channels of this connection, if any. */
  struct rtsp_rc *rc;
  uint16_t rc_event_port;

  /* Buffered APAP + MediaDataControl (lab, streamConnections). */
  bool apap;                  /* current 103 stream uses APAP framing */
  bool mdc;                   /* MediaDataControl channel open */
  uint16_t mdc_port;
  uint64_t mdc_seed;
  int apap_ctrl_sock;         /* UDP controlPort socket, -1 = none */
  uint16_t apap_ctrl_port;
  uint64_t apap_stream_id;
  bool mdc_start_valid;       /* "strt" seen */
  int64_t mdc_media_time_value;
  int64_t mdc_media_time_scale;
  bool mdc_anchor_valid;      /* receiver-chosen anchor ("anch") */
  uint64_t mdc_anchor_ptp_ns;
  uint64_t mdc_anchor_clock_id;
  /* An isRemoteControlOnly session SETUP happened on this connection
   * (lab: its later session TEARDOWN may be meant for that session only). */
  bool rc_session_seen;
  /* Session diagnostics (lab). */
  int64_t diag_setup103_us;   /* type 103 SETUP answered */
  int64_t diag_rc_down_us;    /* type 130 TEARDOWN */
  int64_t diag_last_req_us;
  uint32_t diag_requests;
  uint32_t diag_feedback;
  int64_t diag_last_feedback_us;

  uint32_t client_ip;           // Sender IP (network byte order): PTP peer,
                                // event-port filter, realtime retransmits
  uint16_t client_control_port; // Sender's realtime control port
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
 * Set volume in dB (converts to Q15 internally)
 * @param conn Connection state
 * @param volume_db Volume in dB (0 = max, -144 = mute)
 */
void rtsp_conn_set_volume(rtsp_conn_t *conn, float volume_db);


/* Event volume acknowledgments can race sender SET_PARAMETER. These APIs
 * serialize scalar gain state; the conditional update preserves newer input. */
float rtsp_conn_get_volume_db(rtsp_conn_t *conn);
bool rtsp_conn_set_volume_if_unchanged(rtsp_conn_t *conn, float expected_db,
                                      float volume_db);
