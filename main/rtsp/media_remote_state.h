#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * MediaRemote is context, not an audio clock.
 *
 * This module records what the sender says about the current item, playback
 * state and command capabilities.  It deliberately has no dependency on the
 * audio receiver and must never start, stop, flush or re-anchor audio.  Audio
 * correctness remains owned by the RTSP timing path (FLUSHBUFFERED,
 * SETRATEANCHORTIME and D7 for realtime streams).
 */

#define MEDIA_REMOTE_TITLE_MAX 128
#define MEDIA_REMOTE_TEXT_MAX 128
#define MEDIA_REMOTE_ID_MAX 128
#define MEDIA_REMOTE_COMMAND_MAX 256

typedef enum {
  MEDIA_REMOTE_PLAYBACK_UNKNOWN = 0,
  MEDIA_REMOTE_PLAYBACK_PLAYING,
  MEDIA_REMOTE_PLAYBACK_PAUSED,
  MEDIA_REMOTE_PLAYBACK_STOPPED,
  MEDIA_REMOTE_PLAYBACK_INTERRUPTED,
} media_remote_playback_state_t;

typedef enum {
  MEDIA_REMOTE_SOURCE_NONE = 0,
  MEDIA_REMOTE_SOURCE_FALLBACK,
  MEDIA_REMOTE_SOURCE_MEDIAREMOTE,
} media_remote_metadata_source_t;

typedef struct {
  uint32_t update_serial;
  uint32_t item_serial;
  uint32_t capabilities_serial;
  uint32_t playback_serial;

  media_remote_metadata_source_t metadata_source;
  bool have_media_remote_now_playing;
  bool audio_transport_playing;

  media_remote_playback_state_t reported_playback_state;
  bool have_reported_playback_state;
  bool have_reported_playback_state_raw;
  int64_t reported_playback_state_raw;

  char title[MEDIA_REMOTE_TITLE_MAX];
  char artist[MEDIA_REMOTE_TEXT_MAX];
  char album[MEDIA_REMOTE_TEXT_MAX];
  char genre[MEDIA_REMOTE_TEXT_MAX];
  char composer[MEDIA_REMOTE_TEXT_MAX];
  char content_type[MEDIA_REMOTE_TEXT_MAX];
  char collection_title[MEDIA_REMOTE_TEXT_MAX];
  char item_id[MEDIA_REMOTE_ID_MAX];
  char artwork_id[MEDIA_REMOTE_ID_MAX];

  bool have_duration;
  double duration_secs;
  bool have_elapsed;
  double elapsed_secs;
  bool have_playback_rate;
  double playback_rate;
  bool have_default_playback_rate;
  double default_playback_rate;
  bool have_transition;
  bool in_transition;
  bool have_always_live;
  bool always_live;

  bool have_queue_index;
  int64_t queue_index;
  bool have_queue_count;
  int64_t queue_count;
  bool have_track_number;
  int64_t track_number;
  bool have_track_count;
  int64_t track_count;
  bool have_unique_id;
  int64_t unique_id;

  bool have_artwork_size;
  size_t artwork_bytes;
  bool have_artwork_width;
  int64_t artwork_width;
  bool have_artwork_height;
  int64_t artwork_height;

  size_t advertised_command_count;
  size_t parsed_command_count;
  size_t enabled_command_count;
  bool seek_can_scrub;
  bool seek_supports_reference_position;

  /* 256 command IDs, one bit per MRMediaRemoteCommand value. */
  uint64_t command_present[MEDIA_REMOTE_COMMAND_MAX / 64];
  uint64_t command_enabled[MEDIA_REMOTE_COMMAND_MAX / 64];
} media_remote_state_snapshot_t;

esp_err_t media_remote_state_init(void);

/* Parse one incoming AirPlay POST /command bplist. Unknown command types are
 * intentionally ignored: this state layer is additive and must not alter the
 * receiver's protocol response behaviour. */
void media_remote_state_handle_command(const uint8_t *plist, size_t plist_len);

void media_remote_state_get_snapshot(media_remote_state_snapshot_t *out);

bool media_remote_state_command_present(unsigned command_id);
bool media_remote_state_command_enabled(unsigned command_id);

const char *media_remote_state_playback_name(media_remote_playback_state_t state);
const char *media_remote_state_command_name(unsigned command_id);
