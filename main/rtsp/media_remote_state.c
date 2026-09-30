#include "media_remote_state.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "plist.h"
#include "rtsp_events.h"

static const char *TAG = "media_remote";

#define MR_COMMAND_BLOB_MAX 4096U
#define MR_COMMAND_WORDS (MEDIA_REMOTE_COMMAND_MAX / 64U)

/* Which fields in the current MediaRemote now-playing object were explicitly
 * supplied by the sender. DMAP/legacy metadata may fill only fields that were
 * absent from this mask. */
enum {
  MR_F_TITLE = 1ULL << 0,
  MR_F_ARTIST = 1ULL << 1,
  MR_F_ALBUM = 1ULL << 2,
  MR_F_GENRE = 1ULL << 3,
  MR_F_DURATION = 1ULL << 4,
  MR_F_ELAPSED = 1ULL << 5,
  MR_F_RATE = 1ULL << 6,
};

typedef struct {
  media_remote_state_snapshot_t pub;
  uint64_t mr_field_mask;
  int64_t elapsed_observed_us;
  bool initialized;
} media_remote_state_internal_t;

static media_remote_state_internal_t s_state;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static void copy_text(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0) return;
  if (!src) src = "";
  strlcpy(dst, src, cap);
}

static bool text_equal(const char *a, const char *b) {
  if (!a) a = "";
  if (!b) b = "";
  return strcmp(a, b) == 0;
}

static bool command_bit_get(const uint64_t *bits, unsigned command_id) {
  if (!bits || command_id >= MEDIA_REMOTE_COMMAND_MAX) return false;
  return (bits[command_id / 64U] & (1ULL << (command_id % 64U))) != 0;
}

static void command_bit_set(uint64_t *bits, unsigned command_id) {
  if (!bits || command_id >= MEDIA_REMOTE_COMMAND_MAX) return;
  bits[command_id / 64U] |= 1ULL << (command_id % 64U);
}

const char *media_remote_state_playback_name(media_remote_playback_state_t state) {
  switch (state) {
    case MEDIA_REMOTE_PLAYBACK_PLAYING: return "playing";
    case MEDIA_REMOTE_PLAYBACK_PAUSED: return "paused";
    case MEDIA_REMOTE_PLAYBACK_STOPPED: return "stopped";
    case MEDIA_REMOTE_PLAYBACK_INTERRUPTED: return "interrupted";
    default: return "unknown";
  }
}

const char *media_remote_state_command_name(unsigned command_id) {
  switch (command_id) {
    case 0: return "play";
    case 1: return "pause";
    case 2: return "toggle";
    case 3: return "stop";
    case 4: return "next";
    case 5: return "previous";
    case 6: return "shuffle-next-mode";
    case 7: return "repeat-next-mode";
    case 8: return "begin-fast-forward";
    case 9: return "end-fast-forward";
    case 10: return "begin-rewind";
    case 11: return "end-rewind";
    case 12: return "rewind-15";
    case 13: return "forward-15";
    case 14: return "rewind-30";
    case 15: return "forward-30";
    case 17: return "skip-forward";
    case 18: return "skip-backward";
    case 19: return "playback-rate";
    case 24: return "seek-position";
    case 25: return "repeat-mode";
    case 26: return "shuffle-mode";
    case 27: return "enable-language";
    case 28: return "disable-language";
    case 100: return "next-chapter";
    case 101: return "previous-chapter";
    default: return "other";
  }
}

static media_remote_playback_state_t playback_from_string(const char *value) {
  if (!value || !*value) return MEDIA_REMOTE_PLAYBACK_UNKNOWN;
  if (strcasecmp(value, "playing") == 0 || strcasecmp(value, "play") == 0)
    return MEDIA_REMOTE_PLAYBACK_PLAYING;
  if (strcasecmp(value, "paused") == 0 || strcasecmp(value, "pause") == 0)
    return MEDIA_REMOTE_PLAYBACK_PAUSED;
  if (strcasecmp(value, "stopped") == 0 || strcasecmp(value, "stop") == 0)
    return MEDIA_REMOTE_PLAYBACK_STOPPED;
  if (strcasecmp(value, "interrupted") == 0)
    return MEDIA_REMOTE_PLAYBACK_INTERRUPTED;
  return MEDIA_REMOTE_PLAYBACK_UNKNOWN;
}

/* Apple has used numeric playback-state values as well as descriptive strings
 * in MediaRemote structures. Keep the numeric observation separate from audio
 * transport control; these values are context only. */
static media_remote_playback_state_t playback_from_int(int64_t value) {
  switch (value) {
    case 1: return MEDIA_REMOTE_PLAYBACK_PLAYING;
    case 2: return MEDIA_REMOTE_PLAYBACK_PAUSED;
    case 3: return MEDIA_REMOTE_PLAYBACK_STOPPED;
    case 4: return MEDIA_REMOTE_PLAYBACK_INTERRUPTED;
    default: return MEDIA_REMOTE_PLAYBACK_UNKNOWN;
  }
}

static void state_reset_locked(void) {
  const bool initialized = s_state.initialized;
  memset(&s_state, 0, sizeof(s_state));
  s_state.initialized = initialized;
  s_state.pub.reported_playback_state = MEDIA_REMOTE_PLAYBACK_UNKNOWN;
}

static bool identity_changed_locked(const media_remote_state_snapshot_t *next,
                                    uint64_t next_mask) {
  const media_remote_state_snapshot_t *old = &s_state.pub;
  if (!old->have_media_remote_now_playing) return true;

  if (next->have_unique_id && old->have_unique_id)
    return next->unique_id != old->unique_id;

  if (next->item_id[0] && old->item_id[0])
    return !text_equal(next->item_id, old->item_id);

  const bool next_has_identity =
      (next_mask & (MR_F_TITLE | MR_F_ARTIST | MR_F_ALBUM)) != 0;
  if (!next_has_identity) return false;

  return !text_equal(next->title, old->title) ||
         !text_equal(next->artist, old->artist) ||
         !text_equal(next->album, old->album);
}

static void parse_now_playing(const uint8_t *plist, size_t plist_len) {
  media_remote_state_snapshot_t *next = heap_caps_calloc(
      1, sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!next) next = calloc(1, sizeof(*next));
  if (!next) {
    ESP_LOGW(TAG, "NowPlaying ignored: no state scratch buffer");
    return;
  }
  uint64_t mask = 0;
  char merge_policy[24] = {0};
  const bool patch =
      bplist_find_string_deep(plist, plist_len, "mergePolicy", merge_policy,
                              sizeof(merge_policy)) &&
      strcasecmp(merge_policy, "update") == 0;

  portENTER_CRITICAL(&s_state_lock);
  if (patch && s_state.pub.have_media_remote_now_playing) *next = s_state.pub;
  portEXIT_CRITICAL(&s_state_lock);

  /* A replace is a complete new MediaRemote picture. An update is a sparse
   * timeline patch (common for seek and pause/resume), so absent keys must be
   * preserved in that case. */
  if (!patch) {
    memset(next, 0, sizeof(*next));
    next->reported_playback_state = MEDIA_REMOTE_PLAYBACK_UNKNOWN;
  }

  char value[MEDIA_REMOTE_TEXT_MAX];
  if (bplist_find_string_deep(plist, plist_len,
                              "kMRMediaRemoteNowPlayingInfoTitle", value,
                              sizeof(value))) {
    copy_text(next->title, sizeof(next->title), value);
    mask |= MR_F_TITLE;
  }
  if (bplist_find_string_deep(plist, plist_len,
                              "kMRMediaRemoteNowPlayingInfoArtist", value,
                              sizeof(value))) {
    copy_text(next->artist, sizeof(next->artist), value);
    mask |= MR_F_ARTIST;
  }
  if (bplist_find_string_deep(plist, plist_len,
                              "kMRMediaRemoteNowPlayingInfoAlbum", value,
                              sizeof(value))) {
    copy_text(next->album, sizeof(next->album), value);
    mask |= MR_F_ALBUM;
  }
  if (bplist_find_string_deep(plist, plist_len,
                              "kMRMediaRemoteNowPlayingInfoGenre", value,
                              sizeof(value))) {
    copy_text(next->genre, sizeof(next->genre), value);
    mask |= MR_F_GENRE;
  }
  if (bplist_find_string_deep(plist, plist_len,
                              "kMRMediaRemoteNowPlayingInfoComposer", value,
                              sizeof(value))) {
    copy_text(next->composer, sizeof(next->composer), value);
  }
  if (bplist_find_string_deep(plist, plist_len,
                              "kMRMediaRemoteNowPlayingInfoContentType", value,
                              sizeof(value)) ||
      bplist_find_string_deep(plist, plist_len,
                              "kMRMediaRemoteNowPlayingInfoMediaType", value,
                              sizeof(value))) {
    copy_text(next->content_type, sizeof(next->content_type), value);
  }
  if (bplist_find_string_deep(
          plist, plist_len, "kMRMediaRemoteNowPlayingCollectionInfoKeyTitle",
          value, sizeof(value))) {
    copy_text(next->collection_title, sizeof(next->collection_title), value);
  }
  if (bplist_find_string_deep(
          plist, plist_len,
          "kMRMediaRemoteNowPlayingInfoContentItemIdentifier", value,
          sizeof(value))) {
    copy_text(next->item_id, sizeof(next->item_id), value);
  }
  if (bplist_find_string_deep(plist, plist_len,
                              "kMRMediaRemoteNowPlayingInfoArtworkIdentifier",
                              value, sizeof(value))) {
    copy_text(next->artwork_id, sizeof(next->artwork_id), value);
  }

  double real = 0.0;
  if (bplist_find_real_deep(plist, plist_len,
                            "kMRMediaRemoteNowPlayingInfoDuration", &real)) {
    next->have_duration = true;
    next->duration_secs = real;
    mask |= MR_F_DURATION;
  }
  if (bplist_find_real_deep(plist, plist_len,
                            "kMRMediaRemoteNowPlayingInfoElapsedTime", &real)) {
    next->have_elapsed = true;
    next->elapsed_secs = real;
    mask |= MR_F_ELAPSED;
  }
  if (bplist_find_real_deep(plist, plist_len,
                            "kMRMediaRemoteNowPlayingInfoPlaybackRate", &real)) {
    next->have_playback_rate = true;
    next->playback_rate = real;
    mask |= MR_F_RATE;
  }
  if (bplist_find_real_deep(
          plist, plist_len, "kMRMediaRemoteNowPlayingInfoDefaultPlaybackRate",
          &real)) {
    next->have_default_playback_rate = true;
    next->default_playback_rate = real;
  }

  bool flag = false;
  if (bplist_find_bool_deep(plist, plist_len,
                            "kMRMediaRemoteNowPlayingInfoIsInTransition",
                            &flag)) {
    next->have_transition = true;
    next->in_transition = flag;
  }
  if (bplist_find_bool_deep(plist, plist_len,
                            "kMRMediaRemoteNowPlayingInfoIsAlwaysLive", &flag)) {
    next->have_always_live = true;
    next->always_live = flag;
  }

  int64_t integer = 0;
#define MR_READ_INT(KEY, HAVE_FIELD, FIELD)                                     \
  do {                                                                          \
    if (bplist_find_int_deep(plist, plist_len, (KEY), &integer)) {              \
      next->HAVE_FIELD = true;                                                    \
      next->FIELD = integer;                                                      \
    }                                                                            \
  } while (0)
  MR_READ_INT("kMRMediaRemoteNowPlayingInfoQueueIndex", have_queue_index,
              queue_index);
  MR_READ_INT("kMRMediaRemoteNowPlayingInfoTotalQueueCount", have_queue_count,
              queue_count);
  MR_READ_INT("kMRMediaRemoteNowPlayingInfoTrackNumber", have_track_number,
              track_number);
  MR_READ_INT("kMRMediaRemoteNowPlayingInfoTotalTrackCount", have_track_count,
              track_count);
  MR_READ_INT("kMRMediaRemoteNowPlayingInfoUniqueIdentifier", have_unique_id,
              unique_id);
  MR_READ_INT("kMRMediaRemoteNowPlayingInfoArtworkDataWidth", have_artwork_width,
              artwork_width);
  MR_READ_INT("kMRMediaRemoteNowPlayingInfoArtworkDataHeight",
              have_artwork_height, artwork_height);
#undef MR_READ_INT

  size_t artwork_bytes = 0;
  if (bplist_find_data_len_deep(plist, plist_len,
                                "kMRMediaRemoteNowPlayingInfoArtworkData",
                                &artwork_bytes)) {
    next->have_artwork_size = true;
    next->artwork_bytes = artwork_bytes;
  }

  uint32_t update_serial = 0;
  uint32_t item_serial = 0;
  bool item_changed = false;
  portENTER_CRITICAL(&s_state_lock);
  item_changed = identity_changed_locked(next, mask);

  /* Preserve state that belongs to other MediaRemote sub-protocols. */
  next->command_present[0] = s_state.pub.command_present[0];
  next->command_present[1] = s_state.pub.command_present[1];
  next->command_present[2] = s_state.pub.command_present[2];
  next->command_present[3] = s_state.pub.command_present[3];
  next->command_enabled[0] = s_state.pub.command_enabled[0];
  next->command_enabled[1] = s_state.pub.command_enabled[1];
  next->command_enabled[2] = s_state.pub.command_enabled[2];
  next->command_enabled[3] = s_state.pub.command_enabled[3];
  next->advertised_command_count = s_state.pub.advertised_command_count;
  next->parsed_command_count = s_state.pub.parsed_command_count;
  next->enabled_command_count = s_state.pub.enabled_command_count;
  next->seek_can_scrub = s_state.pub.seek_can_scrub;
  next->seek_supports_reference_position =
      s_state.pub.seek_supports_reference_position;
  next->capabilities_serial = s_state.pub.capabilities_serial;
  next->audio_transport_playing = s_state.pub.audio_transport_playing;
  next->reported_playback_state = s_state.pub.reported_playback_state;
  next->have_reported_playback_state = s_state.pub.have_reported_playback_state;
  next->playback_serial = s_state.pub.playback_serial;

  next->have_media_remote_now_playing = true;
  next->metadata_source = MEDIA_REMOTE_SOURCE_MEDIAREMOTE;
  next->update_serial = s_state.pub.update_serial + 1U;
  next->item_serial = s_state.pub.item_serial + (item_changed ? 1U : 0U);

  s_state.pub = *next;
  if (patch)
    s_state.mr_field_mask |= mask;
  else
    s_state.mr_field_mask = mask;
  if (mask & MR_F_ELAPSED) s_state.elapsed_observed_us = esp_timer_get_time();
  update_serial = s_state.pub.update_serial;
  item_serial = s_state.pub.item_serial;
  portEXIT_CRITICAL(&s_state_lock);

  ESP_LOGI(TAG,
           "NowPlaying #%lu item=%lu%s merge=%s title=\"%s\" rate=%s%.2f "
           "transition=%s",
           (unsigned long)update_serial, (unsigned long)item_serial,
           item_changed ? " new" : "", patch ? "update" : "replace",
           next->title[0] ? next->title : "<none>",
           next->have_playback_rate ? "" : "?",
           next->have_playback_rate ? next->playback_rate : 0.0,
           next->have_transition ? (next->in_transition ? "1" : "0") : "?");
  free(next);
}

static void parse_supported_commands(const uint8_t *plist, size_t plist_len) {
  uint8_t *blob = heap_caps_malloc(MR_COMMAND_BLOB_MAX,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!blob) blob = malloc(MR_COMMAND_BLOB_MAX);
  if (!blob) {
    ESP_LOGW(TAG, "SupportedCommands ignored: no %u-byte scratch buffer",
             (unsigned)MR_COMMAND_BLOB_MAX);
    return;
  }

  uint64_t present[MR_COMMAND_WORDS] = {0};
  uint64_t enabled_bits[MR_COMMAND_WORDS] = {0};
  size_t count = 0;
  size_t parsed = 0;
  size_t enabled_count = 0;
  bool seek_can_scrub = false;
  bool seek_supports_reference_position = false;

  /* Asking for item zero also returns the complete array count. */
  size_t item_len = 0;
  if (!bplist_get_data_array_item_deep(
          plist, plist_len, "mrSupportedCommandsFromSender", 0, blob,
          MR_COMMAND_BLOB_MAX, &item_len, &count)) {
    /* An empty list is valid and means no advertised commands. */
    if (count != 0) {
      ESP_LOGW(TAG, "SupportedCommands: %u entries but first item is unusable",
               (unsigned)count);
      free(blob);
      return;
    }
  }

  for (size_t i = 0; i < count; ++i) {
    if (i != 0) {
      size_t observed_count = 0;
      item_len = 0;
      if (!bplist_get_data_array_item_deep(
              plist, plist_len, "mrSupportedCommandsFromSender", i, blob,
              MR_COMMAND_BLOB_MAX, &item_len, &observed_count)) {
        continue;
      }
    }
    if (item_len < 8U || memcmp(blob, "bplist00", 8U) != 0) continue;

    int64_t command = -1;
    bool is_enabled = false;
    if (!bplist_find_int(blob, item_len, "kCommandInfoCommandKey", &command) ||
        command < 0 || command >= MEDIA_REMOTE_COMMAND_MAX) {
      continue;
    }
    const bool have_enabled =
        bplist_find_bool(blob, item_len, "kCommandInfoEnabledKey", &is_enabled);
    command_bit_set(present, (unsigned)command);
    if (have_enabled && is_enabled) {
      command_bit_set(enabled_bits, (unsigned)command);
      enabled_count++;
    }
    parsed++;

    if (command == 24) {
      bool option = false;
      if (bplist_find_bool_deep(
              blob, item_len,
              "kMRMediaRemoteCommandInfoCanBeControlledByScrubbingKey",
              &option)) {
        seek_can_scrub = option;
      }
      if (bplist_find_bool_deep(
              blob, item_len,
              "kMRMediaRemoteCommandInfoSupportsReferencePosition", &option)) {
        seek_supports_reference_position = option;
      }
    }
  }

  free(blob);

  uint32_t capabilities_serial = 0;
  portENTER_CRITICAL(&s_state_lock);
  memcpy(s_state.pub.command_present, present, sizeof(present));
  memcpy(s_state.pub.command_enabled, enabled_bits, sizeof(enabled_bits));
  s_state.pub.advertised_command_count = count;
  s_state.pub.parsed_command_count = parsed;
  s_state.pub.enabled_command_count = enabled_count;
  s_state.pub.seek_can_scrub = seek_can_scrub;
  s_state.pub.seek_supports_reference_position =
      seek_supports_reference_position;
  s_state.pub.capabilities_serial++;
  s_state.pub.update_serial++;
  capabilities_serial = s_state.pub.capabilities_serial;
  portEXIT_CRITICAL(&s_state_lock);

  ESP_LOGI(TAG,
           "Capabilities #%lu advertised=%u parsed=%u enabled=%u "
           "play=%d pause=%d next=%d prev=%d seek=%d scrub=%d ff=%d rw=%d",
           (unsigned long)capabilities_serial, (unsigned)count,
           (unsigned)parsed, (unsigned)enabled_count,
           command_bit_get(enabled_bits, 0), command_bit_get(enabled_bits, 1),
           command_bit_get(enabled_bits, 4), command_bit_get(enabled_bits, 5),
           command_bit_get(enabled_bits, 24), seek_can_scrub,
           command_bit_get(enabled_bits, 8), command_bit_get(enabled_bits, 10));
}

static void parse_playback_state(const uint8_t *plist, size_t plist_len) {
  media_remote_playback_state_t state = MEDIA_REMOTE_PLAYBACK_UNKNOWN;
  bool have_raw = false;
  int64_t raw_value = 0;
  char text[48] = {0};
  const char *string_keys[] = {"playbackState", "mrPlaybackState", "state",
                               "kMRMediaRemotePlaybackState"};
  for (size_t i = 0; i < sizeof(string_keys) / sizeof(string_keys[0]); ++i) {
    if (bplist_find_string_deep(plist, plist_len, string_keys[i], text,
                                sizeof(text))) {
      state = playback_from_string(text);
      if (state != MEDIA_REMOTE_PLAYBACK_UNKNOWN) break;
    }
  }

  if (state == MEDIA_REMOTE_PLAYBACK_UNKNOWN) {
    const char *int_keys[] = {"mrPlaybackState", "playbackState", "state",
                             "kMRMediaRemotePlaybackState"};
    for (size_t i = 0; i < sizeof(int_keys) / sizeof(int_keys[0]); ++i) {
      if (bplist_find_int_deep(plist, plist_len, int_keys[i], &raw_value)) {
        have_raw = true;
        state = playback_from_int(raw_value);
        break;
      }
    }
  }

  if (state == MEDIA_REMOTE_PLAYBACK_UNKNOWN && !have_raw) {
    ESP_LOGD(TAG, "PlaybackState command received; state encoding not recognised");
    return;
  }

  uint32_t serial = 0;
  portENTER_CRITICAL(&s_state_lock);
  s_state.pub.reported_playback_state = state;
  s_state.pub.have_reported_playback_state = true;
  s_state.pub.have_reported_playback_state_raw = have_raw;
  s_state.pub.reported_playback_state_raw = have_raw ? raw_value : 0;
  s_state.pub.playback_serial++;
  s_state.pub.update_serial++;
  serial = s_state.pub.playback_serial;
  portEXIT_CRITICAL(&s_state_lock);

  if (have_raw) {
    ESP_LOGI(TAG, "PlaybackState #%lu reported=%s raw=%lld (context only)",
             (unsigned long)serial, media_remote_state_playback_name(state),
             (long long)raw_value);
  } else {
    ESP_LOGI(TAG, "PlaybackState #%lu reported=%s (context only)",
             (unsigned long)serial, media_remote_state_playback_name(state));
  }
}

static void merge_fallback_metadata(const rtsp_metadata_t *meta) {
  if (!meta) return;

  portENTER_CRITICAL(&s_state_lock);
  media_remote_state_snapshot_t *dst = &s_state.pub;

  if (!dst->have_media_remote_now_playing) {
    if (meta->title[0]) copy_text(dst->title, sizeof(dst->title), meta->title);
    if (meta->artist[0]) copy_text(dst->artist, sizeof(dst->artist), meta->artist);
    if (meta->album[0]) copy_text(dst->album, sizeof(dst->album), meta->album);
    if (meta->genre[0]) copy_text(dst->genre, sizeof(dst->genre), meta->genre);
    if (meta->duration_secs) {
      dst->have_duration = true;
      dst->duration_secs = (double)meta->duration_secs;
    }
    if (meta->position_secs || meta->duration_secs) {
      dst->have_elapsed = true;
      dst->elapsed_secs = (double)meta->position_secs;
      s_state.elapsed_observed_us = esp_timer_get_time();
    }
    dst->metadata_source = MEDIA_REMOTE_SOURCE_FALLBACK;
    dst->update_serial++;
    portEXIT_CRITICAL(&s_state_lock);
    return;
  }

  /* Once MediaRemote has supplied an item identity, do not let a late DMAP
   * packet from the previous song overwrite it. A matching title (or a DMAP
   * packet without a title, e.g. progress) may fill holes MediaRemote omitted. */
  if ((s_state.mr_field_mask & MR_F_TITLE) && meta->title[0] &&
      strcmp(dst->title, meta->title) != 0) {
    portEXIT_CRITICAL(&s_state_lock);
    return;
  }

  bool changed = false;
  if (!(s_state.mr_field_mask & MR_F_TITLE) && meta->title[0]) {
    copy_text(dst->title, sizeof(dst->title), meta->title);
    changed = true;
  }
  if (!(s_state.mr_field_mask & MR_F_ARTIST) && meta->artist[0]) {
    copy_text(dst->artist, sizeof(dst->artist), meta->artist);
    changed = true;
  }
  if (!(s_state.mr_field_mask & MR_F_ALBUM) && meta->album[0]) {
    copy_text(dst->album, sizeof(dst->album), meta->album);
    changed = true;
  }
  if (!(s_state.mr_field_mask & MR_F_GENRE) && meta->genre[0]) {
    copy_text(dst->genre, sizeof(dst->genre), meta->genre);
    changed = true;
  }
  if (!(s_state.mr_field_mask & MR_F_DURATION) && meta->duration_secs) {
    dst->have_duration = true;
    dst->duration_secs = (double)meta->duration_secs;
    changed = true;
  }
  if (!(s_state.mr_field_mask & MR_F_ELAPSED) &&
      (meta->position_secs || meta->duration_secs)) {
    dst->have_elapsed = true;
    dst->elapsed_secs = (double)meta->position_secs;
    s_state.elapsed_observed_us = esp_timer_get_time();
    changed = true;
  }
  if (changed) dst->update_serial++;
  portEXIT_CRITICAL(&s_state_lock);
}

static void media_remote_event(rtsp_event_t event,
                               const rtsp_event_data_t *data,
                               void *user_data) {
  (void)user_data;
  switch (event) {
    case RTSP_EVENT_PLAYING:
      portENTER_CRITICAL(&s_state_lock);
      s_state.pub.audio_transport_playing = true;
      portEXIT_CRITICAL(&s_state_lock);
      break;
    case RTSP_EVENT_PAUSED:
      portENTER_CRITICAL(&s_state_lock);
      s_state.pub.audio_transport_playing = false;
      portEXIT_CRITICAL(&s_state_lock);
      break;
    case RTSP_EVENT_METADATA:
      if (data) merge_fallback_metadata(&data->metadata);
      break;
    case RTSP_EVENT_DISCONNECTED:
      portENTER_CRITICAL(&s_state_lock);
      state_reset_locked();
      portEXIT_CRITICAL(&s_state_lock);
      ESP_LOGI(TAG, "Session state cleared");
      break;
    default:
      break;
  }
}

esp_err_t media_remote_state_init(void) {
  portENTER_CRITICAL(&s_state_lock);
  if (s_state.initialized) {
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
  }
  state_reset_locked();
  s_state.initialized = true;
  portEXIT_CRITICAL(&s_state_lock);

  if (rtsp_events_register(media_remote_event, NULL) != 0) {
    portENTER_CRITICAL(&s_state_lock);
    s_state.initialized = false;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void media_remote_state_handle_command(const uint8_t *plist, size_t plist_len) {
  if (!plist || plist_len < 8U || memcmp(plist, "bplist00", 8U) != 0) return;

  char type[96] = {0};
  if (!bplist_find_string(plist, plist_len, "type", type, sizeof(type))) return;

  if (strcmp(type, "updateMRNowPlayingInfo") == 0) {
    parse_now_playing(plist, plist_len);
  } else if (strcmp(type, "updateMRSupportedCommands") == 0) {
    parse_supported_commands(plist, plist_len);
  } else if (strcmp(type, "updateMRPlaybackState") == 0) {
    parse_playback_state(plist, plist_len);
  } else if (strcmp(type, "updateMRNowPlayingClient") == 0) {
    /* Recognised deliberately. The client identity is not needed by the audio
     * pipeline, but recognising it keeps this module ready for a later UI /
     * controller layer without treating it as an unknown protocol command. */
    ESP_LOGD(TAG, "NowPlayingClient update received");
  }
}

void media_remote_state_get_snapshot(media_remote_state_snapshot_t *out) {
  if (!out) return;
  portENTER_CRITICAL(&s_state_lock);
  *out = s_state.pub;
  portEXIT_CRITICAL(&s_state_lock);
}

bool media_remote_state_command_present(unsigned command_id) {
  bool result = false;
  portENTER_CRITICAL(&s_state_lock);
  result = command_bit_get(s_state.pub.command_present, command_id);
  portEXIT_CRITICAL(&s_state_lock);
  return result;
}

bool media_remote_state_command_enabled(unsigned command_id) {
  bool result = false;
  portENTER_CRITICAL(&s_state_lock);
  result = command_bit_get(s_state.pub.command_enabled, command_id);
  portEXIT_CRITICAL(&s_state_lock);
  return result;
}
