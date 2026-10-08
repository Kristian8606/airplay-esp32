#include "rtsp_conn.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "amp_control.h"
#include "ptp_clock.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE volume_lock = portMUX_INITIALIZER_UNLOCKED;
/* Volume of the audio owner, shown to other connections (GET_PARAMETER). */
static float s_owner_volume_db = -15.0f;

static void rtsp_conn_cleanup(rtsp_conn_t *conn);


static int32_t volume_db_to_q15(float volume_db){
    if (volume_db <= -30.0f)
        return 0;
    if (volume_db >= 0.0f)
        return 32768;
    float normalized = (volume_db + 30.0f) / 30.0f;
    float curved = normalized * normalized;

    return (int32_t)(curved * 32768.0f);
}

rtsp_conn_t *rtsp_conn_create(void) {
  rtsp_conn_t *conn = calloc(1, sizeof(rtsp_conn_t));
  if (!conn) {
    return NULL;
  }

  /* The saved volume is loaded and applied when this connection becomes the
   * audio owner (rtsp_conn_load_volume): a remote-control or /info-only
   * connection must not reset the volume of a session that is playing. */
  conn->volume_db = s_owner_volume_db;

  conn->event_socket = -1;
  conn->rc_event_socket = -1;

  return conn;
}

void rtsp_conn_load_volume(rtsp_conn_t *conn) {
  if (!conn) return;
  // Load saved AirPlay volume or use a conservative default.
  float saved_volume;
  if (settings_get_volume(&saved_volume) == ESP_OK) {
    conn->volume_db = saved_volume;
  } else {
    conn->volume_db = -15.0f;
  }
  s_owner_volume_db = conn->volume_db;
  audio_receiver_set_volume_q15(volume_db_to_q15(conn->volume_db));
}

void rtsp_conn_free(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  /* The RTSP connection owns exactly one amplifier session reference.
   * Stream TEARDOWN intentionally does not touch it: only destruction of the
   * control connection starts the amplifier power-off grace period. */
  if (conn->amp_session_active) {
    conn->amp_session_active = false;
    amp_control_session_disconnected();
  }

  // Persist volume at disconnect (only the audio owner changes it)
  if (conn->owns_audio) settings_persist_volume();

  // Cleanup any resources
  rtsp_conn_cleanup(conn);

  // Free HAP session if present
  if (conn->hap_session) {
    hap_session_free(conn->hap_session);
    conn->hap_session = NULL;
  }

  free(conn->crypto_scratch);
  free(conn);
}

/* Full cleanup when the connection closes: closes sockets, clears PTP. */
static void rtsp_conn_cleanup(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Note: audio_receiver_stop() is NOT called here — it is a global operation
  // and must be managed by the caller (rtsp_server cleanup / handle_teardown)
  // to avoid killing a new session's audio during client replacement.

  // Close sockets
  if (conn->event_socket >= 0) {
    close(conn->event_socket);
    conn->event_socket = -1;
  }
  if (conn->rc_event_socket >= 0) {
    close(conn->rc_event_socket);
    conn->rc_event_socket = -1;
  }
  conn->rc_event_port = 0;

  // Reset stream state
  conn->stream_active = false;
  conn->stream_paused = false;
  conn->data_port = 0;
  conn->control_port = 0;
  conn->event_port = 0;
  conn->buffered_port = 0;

  // Connection teardown ends the lifetime of SETPEERS/SETPEERSX metadata.
  // Stream-level TEARDOWN keeps the RTSP connection alive and therefore does
  // not come through this cleanup path until the session actually closes.
  // PTP is global: only the audio owner may reset it (a remote-control or
  // /info-only connection closing must not disturb the playing session).
  if (conn->owns_audio) {
    ptp_clock_set_peers(NULL, 0);

    // Clear PTP clock for fresh sync on next connection
    ptp_clock_clear();
  }
  conn->ptp_session_fresh = false;

  // Reset encryption state
  conn->encrypted_mode = false;
}

static bool conn_update_volume(rtsp_conn_t *conn, float volume_db,
                               bool conditional, float expected_db) {
  if (!conn || !isfinite(volume_db)) return false;
  if (volume_db > 0.0f) volume_db = 0.0f;
  if (volume_db < -144.0f) volume_db = -144.0f;
  int32_t gain = volume_db_to_q15(volume_db);
  portENTER_CRITICAL(&volume_lock);
  if (conditional && conn->volume_db != expected_db) {
    portEXIT_CRITICAL(&volume_lock);
    return false;
  }
  conn->volume_db = volume_db;
  if (conn->owns_audio) s_owner_volume_db = volume_db;
  audio_receiver_set_volume_q15(gain); /* Atomic target; existing output ramp. */
  /* This setter only updates the cached float; NVS is written at disconnect. */
  settings_set_volume(volume_db);
  portEXIT_CRITICAL(&volume_lock);
  return true;
}

void rtsp_conn_set_volume(rtsp_conn_t *conn, float volume_db) {
  conn_update_volume(conn, volume_db, false, 0.0f);
}

bool rtsp_conn_set_volume_if_unchanged(rtsp_conn_t *conn, float expected_db,
                                      float volume_db) {
  return conn_update_volume(conn, volume_db, true, expected_db);
}

float rtsp_conn_get_volume_db(rtsp_conn_t *conn) {
  if (!conn) return -144.0f;
  portENTER_CRITICAL(&volume_lock);
  float volume = conn->volume_db;
  portEXIT_CRITICAL(&volume_lock);
  return volume;
}

