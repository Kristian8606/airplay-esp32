#include "rtsp_conn.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "amp_control.h"
#include "ptp_clock.h"
#include "settings.h"
#include "rtsp_datastream.h"
#include "rtsp_apap_audio.h"
#include "ap2_control_watch.h"


/* Diagnostic connection IDs are logging-only. Atomic increment avoids duplicate
 * IDs when two accepted RTSP client tasks start at nearly the same time. */
static uint32_t s_diag_conn_serial;

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

  conn->diag_conn_id =
      __atomic_add_fetch(&s_diag_conn_serial, 1U, __ATOMIC_RELAXED);
  if (conn->diag_conn_id == 0) {
    conn->diag_conn_id =
        __atomic_add_fetch(&s_diag_conn_serial, 1U, __ATOMIC_RELAXED);
  }

  // Load saved AirPlay volume or use a conservative default.
  float saved_volume;
  if (settings_get_volume(&saved_volume) == ESP_OK) {
    conn->volume_db = saved_volume;
  } else {
    conn->volume_db = -15.0f;
  }
  conn->volume_q15 = volume_db_to_q15(conn->volume_db);
  /* The output volume is applied when this connection acquires the play
   * lock (rtsp_server_acquire_play_lock); merely connecting must not change
   * the volume of a session that is already playing. */

  conn->event_socket = -1;
  conn->apap_control_socket = -1;

  return conn;
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

  // Persist volume at disconnect (only the playing connection owns it)
  if (conn->play_owner) {
    settings_persist_volume();
  }

  // Cleanup any resources
  rtsp_conn_cleanup(conn);

  // Free HAP session if present
  if (conn->hap_session) {
    hap_session_free(conn->hap_session);
    conn->hap_session = NULL;
  }

  free(conn);
}

void rtsp_conn_cleanup(rtsp_conn_t *conn) {
  if (!conn) {
    return;
  }

  // Note: audio_receiver_stop() is NOT called here — it is a global operation
  // and must be managed by the caller (rtsp_server cleanup / handle_teardown)
  // to avoid killing a new session's audio during client replacement.

  if (conn->event_socket >= 0) {
    close(conn->event_socket);
    conn->event_socket = -1;
  }
  if (conn->apap_control_socket >= 0) {
    rtsp_conn_close_apap_control(conn);
  }
  rtsp_datastream_stop(&conn->media_data_control);
  rtsp_datastream_stop(&conn->remote_control_datastream);
  rtsp_apap_audio_stop(&conn->apap_audio);
  conn->media_data_control_port = 0;
  conn->remote_control_data_port = 0;
  conn->apap_port = 0;

  // Reset stream state
  conn->stream_active = false;
  conn->stream_paused = false;
  conn->data_port = 0;
  conn->control_port = 0;
  conn->event_port = 0;
  conn->stream_id = 0;

  // Connection teardown ends the lifetime of SETPEERS/SETPEERSX metadata.
  // Stream-level TEARDOWN keeps the RTSP connection alive and therefore does
  // not come through this cleanup path until the session actually closes.
  // PTP is global: only the playing connection may reset it.
  if (conn->play_owner) {
    ptp_clock_set_peers(NULL, 0);
    // Clear PTP clock for fresh sync on next connection
    ptp_clock_clear();
  }
  conn->ptp_session_fresh = false;

  // Reset encryption state
  conn->encrypted_mode = false;
}

void rtsp_conn_set_volume(rtsp_conn_t *conn, float volume_db) {
  if (!conn) {
    return;
  }

  // AirPlay volume in dB: 0 = full scale; -30 and below (-144 = mute) are silent.
  if (volume_db > 0.0f) volume_db = 0.0f;
  if (volume_db < -144.0f) volume_db = -144.0f;
  conn->volume_db = volume_db;
  conn->volume_q15 = volume_db_to_q15(volume_db);
  if (!conn->play_owner) {
    return; // remembered for this connection; applied if it starts playing
  }
  audio_receiver_set_volume_q15(conn->volume_q15);

  // Persist at disconnect.
  settings_set_volume(volume_db);
}

void rtsp_conn_close_apap_control(rtsp_conn_t *conn) {
  if (!conn || conn->apap_control_socket < 0) return;
  ap2_control_unwatch(conn->apap_control_socket);
  close(conn->apap_control_socket);
  conn->apap_control_socket = -1;
}
