#include "rtsp_protocol_trace.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "airplay_features.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "plist.h"
#include "sdkconfig.h"

#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE

static const char *TAG = "ap2_trace";

#define TRACE_MRP_DATA_MAX 16384U
#define TRACE_HEX_BYTES 64U
#define TRACE_MR_COMMAND_MAX 4096U
#define TRACE_MR_COMMAND_TEXT_MAX 2048U
#define TRACE_MR_COMMAND_COUNT_MAX 96U

static bool read_varint(const uint8_t *buf, size_t len, size_t *pos,
                        uint64_t *out) {
  uint64_t v = 0;
  unsigned shift = 0;
  size_t p = *pos;
  while (p < len && shift < 64) {
    const uint8_t b = buf[p++];
    v |= (uint64_t)(b & 0x7fU) << shift;
    if ((b & 0x80U) == 0) {
      *pos = p;
      *out = v;
      return true;
    }
    shift += 7;
  }
  return false;
}

static const char *mrp_type_name(uint64_t type) {
  switch (type) {
    case 0: return "UNKNOWN_MESSAGE";
    case 1: return "SEND_COMMAND_MESSAGE";
    case 2: return "SEND_COMMAND_RESULT_MESSAGE";
    case 3: return "GET_STATE_MESSAGE";
    case 4: return "SET_STATE_MESSAGE";
    case 5: return "SET_ARTWORK_MESSAGE";
    case 6: return "REGISTER_HID_DEVICE_MESSAGE";
    case 7: return "REGISTER_HID_DEVICE_RESULT_MESSAGE";
    case 8: return "SEND_HID_EVENT_MESSAGE";
    case 9: return "SEND_HID_REPORT_MESSAGE";
    case 10: return "SEND_VIRTUAL_TOUCH_EVENT_MESSAGE";
    case 11: return "NOTIFICATION_MESSAGE";
    case 12: return "CONTENT_ITEMS_CHANGED_NOTIFICATION_MESSAGE";
    case 15: return "DEVICE_INFO_MESSAGE";
    case 16: return "CLIENT_UPDATES_CONFIG_MESSAGE";
    case 17: return "VOLUME_CONTROL_AVAILABILITY_MESSAGE";
    case 18: return "GAME_CONTROLLER_MESSAGE";
    case 19: return "REGISTER_GAME_CONTROLLER_MESSAGE";
    case 20: return "REGISTER_GAME_CONTROLLER_RESPONSE_MESSAGE";
    case 21: return "UNREGISTER_GAME_CONTROLLER_MESSAGE";
    case 22: return "REGISTER_FOR_GAME_CONTROLLER_EVENTS_MESSAGE";
    case 23: return "KEYBOARD_MESSAGE";
    case 24: return "GET_KEYBOARD_SESSION_MESSAGE";
    case 25: return "TEXT_INPUT_MESSAGE";
    case 26: return "GET_VOICE_INPUT_DEVICES_MESSAGE";
    case 27: return "GET_VOICE_INPUT_DEVICES_RESPONSE_MESSAGE";
    case 28: return "REGISTER_VOICE_INPUT_DEVICE_MESSAGE";
    case 29: return "REGISTER_VOICE_INPUT_DEVICE_RESPONSE_MESSAGE";
    case 30: return "SET_RECORDING_STATE_MESSAGE";
    case 31: return "SEND_VOICE_INPUT_MESSAGE";
    case 32: return "PLAYBACK_QUEUE_REQUEST_MESSAGE";
    case 33: return "TRANSACTION_MESSAGE";
    case 34: return "CRYPTO_PAIRING_MESSAGE";
    case 35: return "GAME_CONTROLLER_PROPERTIES_MESSAGE";
    case 36: return "SET_READY_STATE_MESSAGE";
    case 37: return "DEVICE_INFO_UPDATE_MESSAGE";
    case 38: return "SET_CONNECTION_STATE_MESSAGE";
    case 39: return "SEND_BUTTON_EVENT_MESSAGE";
    case 40: return "SET_HILITE_MODE_MESSAGE";
    case 41: return "WAKE_DEVICE_MESSAGE";
    case 42: return "GENERIC_MESSAGE";
    case 43: return "SEND_PACKED_VIRTUAL_TOUCH_EVENT_MESSAGE";
    case 44: return "SEND_LYRICS_EVENT";
    case 46: return "SET_NOW_PLAYING_CLIENT_MESSAGE";
    case 47: return "SET_NOW_PLAYING_PLAYER_MESSAGE";
    case 48: return "MODIFY_OUTPUT_CONTEXT_REQUEST_MESSAGE";
    case 49: return "GET_VOLUME_MESSAGE";
    case 50: return "GET_VOLUME_RESULT_MESSAGE";
    case 51: return "SET_VOLUME_MESSAGE";
    case 52: return "VOLUME_DID_CHANGE_MESSAGE";
    case 53: return "REMOVE_CLIENT_MESSAGE";
    case 54: return "REMOVE_PLAYER_MESSAGE";
    case 55: return "UPDATE_CLIENT_MESSAGE";
    case 56: return "UPDATE_CONTENT_ITEM_MESSAGE";
    case 57: return "UPDATE_CONTENT_ITEM_ARTWORK_MESSAGE";
    case 58: return "UPDATE_PLAYER_MESSAGE";
    case 59: return "PROMPT_FOR_ROUTE_AUTHORIZATION_MESSAGE";
    case 60: return "PROMPT_FOR_ROUTE_AUTHORIZATION_RESPONSE_MESSAGE";
    case 61: return "PRESENT_ROUTE_AUTHORIZATION_STATUS_MESSAGE";
    case 62: return "GET_VOLUME_CONTROL_CAPABILITIES_MESSAGE";
    case 63: return "GET_VOLUME_CONTROL_CAPABILITIES_RESULT_MESSAGE";
    case 64: return "VOLUME_CONTROL_CAPABILITIES_DID_CHANGE_MESSAGE";
    case 65: return "UPDATE_OUTPUT_DEVICE_MESSAGE";
    case 66: return "REMOVE_OUTPUT_DEVICES_MESSAGE";
    case 67: return "REMOTE_TEXT_INPUT_MESSAGE";
    case 68: return "GET_REMOTE_TEXT_INPUT_SESSION_MESSAGE";
    case 69: return "REMOVE_OUTPUT_DEVICES_MESSAGE2";
    case 70: return "PLAYBACK_SESSION_REQUEST_MESSAGE";
    case 71: return "PLAYBACK_SESSION_RESPONSE_MESSAGE";
    case 72: return "SET_DEFAULT_SUPPORTED_COMMANDS_MESSAGE";
    case 73: return "PLAYBACK_SESSION_MIGRATE_REQUEST_MESSAGE";
    case 74: return "PLAYBACK_SESSION_MIGRATE_RESPONSE_MESSAGE";
    case 75: return "PLAYBACK_SESSION_MIGRATE_BEGIN_MESSAGE";
    case 76: return "PLAYBACK_SESSION_MIGRATE_END_MESSAGE";
    case 77: return "UPDATE_ACTIVE_SYSTEM_ENDPOINT_MESSAGE";
    case 101: return "SET_DISCOVERY_MODE_MESSAGE";
    case 102: return "UPDATE_END_POINTS_MESSAGE";
    case 103: return "REMOVE_ENDPOINTS_MESSAGE";
    case 104: return "PLAYER_CLIENT_PROPERTIES_MESSAGE";
    case 105: return "ORIGIN_CLIENT_PROPERTIES_MESSAGE";
    case 106: return "AUDIO_FADE_MESSAGE";
    case 107: return "AUDIO_FADE_RESPONSE_MESSAGE";
    case 120: return "CONFIGURE_CONNECTION_MESSAGE";
    default: return "unmapped";
  }
}

static void hex_prefix(const char *label, const uint8_t *data, size_t len) {
  if (!data || !len) return;
  const size_t n = len < TRACE_HEX_BYTES ? len : TRACE_HEX_BYTES;
  char text[TRACE_HEX_BYTES * 3U + 8U];
  size_t p = 0;
  for (size_t i = 0; i < n && p + 4U < sizeof(text); ++i) {
    const int w = snprintf(text + p, sizeof(text) - p, "%02X%s", data[i],
                           i + 1U == n ? "" : " ");
    if (w < 0) break;
    p += (size_t)w;
  }
  text[sizeof(text) - 1U] = '\0';
  ESP_LOGI(TAG, "%s hex[%u/%u]=%s%s", label, (unsigned)n, (unsigned)len,
           text, len > n ? " ..." : "");
}

/* Decode enough of a ProtocolMessage envelope to discover new MediaRemote
 * traffic without importing protobuf into the firmware. Unknown extensions
 * are intentionally reported by field number and wire length. */
static bool trace_one_protocol_message(const char *label, const uint8_t *buf,
                                       size_t len, unsigned index) {
  size_t pos = 0;
  bool saw_field = false;
  bool have_type = false;
  uint64_t type = 0;
  char identifier[96] = {0};
  char fields[256] = {0};
  size_t fp = 0;
  unsigned field_count = 0;

  while (pos < len && field_count < 64U) {
    uint64_t key = 0;
    if (!read_varint(buf, len, &pos, &key) || key == 0) return false;
    const uint32_t field = (uint32_t)(key >> 3);
    const uint8_t wire = (uint8_t)(key & 7U);
    if (field == 0) return false;
    saw_field = true;
    field_count++;

    uint64_t v = 0;
    size_t item_len = 0;
    const uint8_t *item = NULL;
    switch (wire) {
      case 0:
        if (!read_varint(buf, len, &pos, &v)) return false;
        if (field == 1) {
          have_type = true;
          type = v;
        }
        break;
      case 1:
        if (len - pos < 8U) return false;
        item = buf + pos;
        item_len = 8U;
        pos += 8U;
        break;
      case 2: {
        uint64_t n = 0;
        if (!read_varint(buf, len, &pos, &n) || n > len - pos) return false;
        item = buf + pos;
        item_len = (size_t)n;
        if (field == 2 && item_len > 0) {
          const size_t copy = item_len < sizeof(identifier) - 1U
                                  ? item_len : sizeof(identifier) - 1U;
          bool printable = true;
          for (size_t i = 0; i < copy; ++i) {
            if (item[i] < 0x20 || item[i] > 0x7e) {
              printable = false;
              break;
            }
          }
          if (printable) {
            memcpy(identifier, item, copy);
            identifier[copy] = '\0';
          }
        }
        pos += item_len;
        break;
      }
      case 5:
        if (len - pos < 4U) return false;
        item = buf + pos;
        item_len = 4U;
        pos += 4U;
        break;
      default:
        return false;
    }

    if (field >= 6U && field != 78U && field != 85U && fp + 24U < sizeof(fields)) {
      const int w = snprintf(fields + fp, sizeof(fields) - fp, "%s%u/w%u/%u",
                             fp ? "," : "", (unsigned)field, (unsigned)wire,
                             (unsigned)item_len);
      if (w > 0) fp += (size_t)w;
    }
    (void)item;
  }

  if (!saw_field || !have_type) return false;
  ESP_LOGI(TAG,
           "%s MRP[%u]: type=%" PRIu64 "(%s) id=\"%s\" bytes=%u ext=[%s]",
           label, index, type, mrp_type_name(type), identifier,
           (unsigned)len, fields);
  return true;
}

static bool trace_mrp_blob(const char *label, const uint8_t *data, size_t len) {
  if (!data || len == 0) return false;

  /* DataStream's params.data normally contains one or more varint-length
   * prefixed ProtocolMessage blobs. CONFIGURE_CONNECTION can also appear
   * unprefixed, so fall back to parsing the complete blob. */
  size_t pos = 0;
  unsigned count = 0;
  bool any = false;
  while (pos < len && count < 32U) {
    const size_t prefix_pos = pos;
    uint64_t n = 0;
    if (!read_varint(data, len, &pos, &n) || n == 0 || n > len - pos) {
      pos = prefix_pos;
      break;
    }
    if (data[pos] != 0x08U) {
      pos = prefix_pos;
      break;
    }
    if (!trace_one_protocol_message(label, data + pos, (size_t)n, count)) {
      pos = prefix_pos;
      break;
    }
    any = true;
    count++;
    pos += (size_t)n;
  }
  if (any && pos == len) return true;

  if (trace_one_protocol_message(label, data, len, 0)) return true;
  return any;
}


#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE_MR_COMMANDS
static uint32_t trace_hash32(const uint8_t *data, size_t len) {
  uint32_t h = 2166136261U;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619U;
  }
  return h;
}
#endif


static void trace_format_time(double seconds, char *out, size_t out_cap) {
  if (!out || out_cap == 0) return;
  if (seconds < 0) seconds = 0;
  const unsigned whole = (unsigned)seconds;
  const unsigned minutes = whole / 60U;
  const unsigned secs = whole % 60U;
  unsigned millis = (unsigned)((seconds - (double)whole) * 1000.0 + 0.5);
  if (millis >= 1000U) millis = 999U;
  snprintf(out, out_cap, "%u:%02u.%03u", minutes, secs, millis);
}

static const char *trace_yes_no(bool value) {
  return value ? "yes" : "no";
}

static void trace_trim_copy(char *dst, size_t dst_cap, const char *src,
                            size_t max_chars) {
  if (!dst || dst_cap == 0) return;
  dst[0] = '\0';
  if (!src) return;
  const size_t src_len = strlen(src);
  size_t copy = src_len;
  if (copy > max_chars) copy = max_chars;
  if (copy > dst_cap - 1U) copy = dst_cap - 1U;
  memcpy(dst, src, copy);
  dst[copy] = '\0';
  if (src_len > copy && dst_cap >= 4U) {
    size_t end = strlen(dst);
    if (end + 3U < dst_cap) {
      memcpy(dst + end, "...", 4U);
    }
  }
}

static void trace_now_playing_info(const uint8_t *plist, size_t plist_len) {
  char title[256] = {0};
  char artist[256] = {0};
  char album[256] = {0};
  char genre[128] = {0};
  char composer[256] = {0};
  char content_type[96] = {0};
  char collection_title[192] = {0};
  char collection_type[192] = {0};
  char item_id[192] = {0};
  char artwork_id[192] = {0};

  const bool have_title = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoTitle",
      title, sizeof(title));
  const bool have_artist = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoArtist",
      artist, sizeof(artist));
  const bool have_album = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoAlbum",
      album, sizeof(album));
  const bool have_genre = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoGenre",
      genre, sizeof(genre));
  const bool have_composer = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoComposer",
      composer, sizeof(composer));
  const bool have_content_type = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoContentType",
      content_type, sizeof(content_type));
  const bool have_collection_title = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingCollectionInfoKeyTitle",
      collection_title, sizeof(collection_title));
  const bool have_collection_type = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingCollectionInfoKeyCollectionType",
      collection_type, sizeof(collection_type));
  const bool have_item_id = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoContentItemIdentifier",
      item_id, sizeof(item_id));
  const bool have_artwork_id = bplist_find_string_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoArtworkIdentifier",
      artwork_id, sizeof(artwork_id));

  double elapsed = 0.0;
  double duration = 0.0;
  double playback_rate = 0.0;
  const bool have_elapsed = bplist_find_real_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoElapsedTime", &elapsed);
  const bool have_duration = bplist_find_real_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoDuration", &duration);
  const bool have_rate = bplist_find_real_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoPlaybackRate",
      &playback_rate);

  int64_t queue_index = -1;
  int64_t queue_count = -1;
  int64_t track_number = -1;
  int64_t track_count = -1;
  int64_t artwork_width = -1;
  int64_t artwork_height = -1;
  int64_t store_id = -1;
  int64_t unique_id = -1;
  const bool have_queue_index = bplist_find_int_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoQueueIndex", &queue_index);
  const bool have_queue_count = bplist_find_int_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoTotalQueueCount",
      &queue_count);
  const bool have_track_number = bplist_find_int_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoTrackNumber",
      &track_number);
  const bool have_track_count = bplist_find_int_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoTotalTrackCount",
      &track_count);
  const bool have_artwork_width = bplist_find_int_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoArtworkDataWidth",
      &artwork_width);
  const bool have_artwork_height = bplist_find_int_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoArtworkDataHeight",
      &artwork_height);
  const bool have_store_id = bplist_find_int_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoiTunesStoreIdentifier",
      &store_id);
  const bool have_unique_id = bplist_find_int_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoUniqueIdentifier",
      &unique_id);

  bool in_transition = false;
  bool always_live = false;
  const bool have_transition = bplist_find_bool_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoIsInTransition",
      &in_transition);
  const bool have_live = bplist_find_bool_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoIsAlwaysLive",
      &always_live);

  size_t artwork_bytes = 0;
  const bool have_artwork_bytes = bplist_find_data_len_deep(
      plist, plist_len, "kMRMediaRemoteNowPlayingInfoArtworkData",
      &artwork_bytes);

  ESP_LOGI(TAG, "PTRACE NOW PLAYING ------------------------------------------------");
  if (have_title || have_artist || have_album) {
    ESP_LOGI(TAG, "PTRACE   Track   : %s", have_title ? title : "<unknown>");
    if (have_artist) ESP_LOGI(TAG, "PTRACE   Artist  : %s", artist);
    if (have_album) ESP_LOGI(TAG, "PTRACE   Album   : %s", album);
  } else {
    ESP_LOGI(TAG, "PTRACE   Track   : <metadata cleared / no title>");
  }

  if (have_elapsed || have_duration || have_rate || have_transition) {
    char elapsed_text[24] = "--:--";
    char duration_text[24] = "--:--";
    if (have_elapsed) trace_format_time(elapsed, elapsed_text, sizeof(elapsed_text));
    if (have_duration) trace_format_time(duration, duration_text, sizeof(duration_text));
    ESP_LOGI(TAG,
             "PTRACE   Time    : %s / %s | rate=%s%.2f | transition=%s",
             elapsed_text, duration_text, have_rate ? "" : "?",
             have_rate ? playback_rate : 0.0,
             have_transition ? trace_yes_no(in_transition) : "?");
  }

  if (have_queue_index || have_queue_count || have_track_number ||
      have_track_count) {
    ESP_LOGI(TAG,
             "PTRACE   Queue   : index=%lld/%lld | track=%lld/%lld",
             (long long)(have_queue_index ? queue_index : -1),
             (long long)(have_queue_count ? queue_count : -1),
             (long long)(have_track_number ? track_number : -1),
             (long long)(have_track_count ? track_count : -1));
  }

  if (have_genre || have_composer) {
    if (have_genre && have_composer) {
      ESP_LOGI(TAG, "PTRACE   Details : genre=%s | composer=%s", genre,
               composer);
    } else if (have_genre) {
      ESP_LOGI(TAG, "PTRACE   Details : genre=%s", genre);
    } else {
      ESP_LOGI(TAG, "PTRACE   Details : composer=%s", composer);
    }
  }

  if (have_collection_title || have_collection_type || have_content_type ||
      have_live) {
    char collection_short[96] = {0};
    trace_trim_copy(collection_short, sizeof(collection_short),
                    have_collection_type ? collection_type : "", 80U);
    ESP_LOGI(TAG,
             "PTRACE   Source  : content=%s | collection=%s%s%s | live=%s",
             have_content_type ? content_type : "?",
             have_collection_title ? "\"" : "",
             have_collection_title ? collection_title :
                 (have_collection_type ? collection_short : "?"),
             have_collection_title ? "\"" : "",
             have_live ? trace_yes_no(always_live) : "?");
    if (have_collection_title && have_collection_type) {
      ESP_LOGI(TAG, "PTRACE             collection-type=%s",
               collection_short);
    }
  }

  if (have_artwork_width || have_artwork_height || have_artwork_bytes ||
      have_artwork_id) {
    char art_id_short[96] = {0};
    trace_trim_copy(art_id_short, sizeof(art_id_short),
                    have_artwork_id ? artwork_id : "", 80U);
    ESP_LOGI(TAG,
             "PTRACE   Artwork : %lldx%lld | data=%s%uB%s | id=%s",
             (long long)(have_artwork_width ? artwork_width : -1),
             (long long)(have_artwork_height ? artwork_height : -1),
             have_artwork_bytes ? "" : "?",
             (unsigned)(have_artwork_bytes ? artwork_bytes : 0U),
             have_artwork_bytes ? " (omitted)" : "",
             have_artwork_id ? art_id_short : "?");
  }

  if (have_item_id || have_store_id || have_unique_id) {
    char item_short[96] = {0};
    trace_trim_copy(item_short, sizeof(item_short),
                    have_item_id ? item_id : "", 80U);
    ESP_LOGI(TAG,
             "PTRACE   IDs     : item=%s | store=%lld | unique=%lld",
             have_item_id ? item_short : "?",
             (long long)(have_store_id ? store_id : -1),
             (long long)(have_unique_id ? unique_id : -1));
  }
  ESP_LOGI(TAG, "PTRACE ------------------------------------------------------------");
}

#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE_MR_COMMANDS
static void trace_mr_supported_commands(const uint8_t *plist,
                                        size_t plist_len) {
  uint8_t *blob = heap_caps_malloc(TRACE_MR_COMMAND_MAX,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  char *desc = heap_caps_malloc(TRACE_MR_COMMAND_TEXT_MAX,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!blob || !desc) {
    heap_caps_free(desc);
    heap_caps_free(blob);
    ESP_LOGW(TAG, "PTRACE MR-supported: trace allocation failed");
    return;
  }

  size_t item_len = 0;
  size_t count = 0;
  if (!bplist_get_data_array_item_deep(
          plist, plist_len, "mrSupportedCommandsFromSender", 0, blob,
          TRACE_MR_COMMAND_MAX, &item_len, &count)) {
    if (count == 0) {
      ESP_LOGI(TAG, "PTRACE MR-supported count=0");
    } else {
      ESP_LOGW(TAG,
               "PTRACE MR-supported count=%u but item[0] is missing, not DATA, "
               "or >%uB",
               (unsigned)count, (unsigned)TRACE_MR_COMMAND_MAX);
    }
    heap_caps_free(desc);
    heap_caps_free(blob);
    return;
  }

  ESP_LOGI(TAG, "PTRACE MR-supported count=%u", (unsigned)count);
  const size_t limit =
      count < TRACE_MR_COMMAND_COUNT_MAX ? count : TRACE_MR_COMMAND_COUNT_MAX;
  for (size_t i = 0; i < limit; ++i) {
    if (i != 0) {
      item_len = 0;
      size_t observed_count = 0;
      if (!bplist_get_data_array_item_deep(
              plist, plist_len, "mrSupportedCommandsFromSender", i, blob,
              TRACE_MR_COMMAND_MAX, &item_len, &observed_count)) {
        ESP_LOGW(TAG,
                 "PTRACE MRcmd[%u/%u]: missing, not DATA, or >%uB",
                 (unsigned)i, (unsigned)count,
                 (unsigned)TRACE_MR_COMMAND_MAX);
        continue;
      }
    }

    const uint32_t hash = trace_hash32(blob, item_len);
    int64_t command = -1;
    bool enabled = false;
    const bool have_command =
        item_len >= 8U && memcmp(blob, "bplist00", 8) == 0 &&
        bplist_find_int(blob, item_len, "kCommandInfoCommandKey", &command);
    const bool have_enabled =
        item_len >= 8U && memcmp(blob, "bplist00", 8) == 0 &&
        bplist_find_bool(blob, item_len, "kCommandInfoEnabledKey", &enabled);

    desc[0] = '\0';
    const size_t described =
        item_len >= 8U && memcmp(blob, "bplist00", 8) == 0
            ? bplist_describe(blob, item_len, desc, TRACE_MR_COMMAND_TEXT_MAX)
            : 0;
    if (described > 0) {
      ESP_LOGI(TAG,
               "PTRACE MRcmd[%u/%u] cmd=%lld enabled=%s size=%u "
               "hash=%08" PRIx32 " %s",
               (unsigned)i, (unsigned)count,
               (long long)(have_command ? command : -1),
               have_enabled ? (enabled ? "1" : "0") : "?",
               (unsigned)item_len, hash, desc);
    } else {
      ESP_LOGI(TAG,
               "PTRACE MRcmd[%u/%u] cmd=%lld enabled=%s size=%u "
               "hash=%08" PRIx32 " <non-bplist>",
               (unsigned)i, (unsigned)count,
               (long long)(have_command ? command : -1),
               have_enabled ? (enabled ? "1" : "0") : "?",
               (unsigned)item_len, hash);
      hex_prefix("PTRACE MRcmd", blob, item_len);
    }
  }

  if (count > limit) {
    ESP_LOGW(TAG, "PTRACE MR-supported truncated at %u/%u entries",
             (unsigned)limit, (unsigned)count);
  }
  heap_caps_free(desc);
  heap_caps_free(blob);
}

#endif /* CONFIG_AIRPLAY_PROTOCOL_TRACE_MR_COMMANDS */

static void trace_bplist_embedded_data(const char *label,
                                       const uint8_t *plist,
                                       size_t plist_len) {
  uint8_t *data = heap_caps_malloc(TRACE_MRP_DATA_MAX,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!data) return;
  size_t data_len = 0;
  if (bplist_find_data_deep(plist, plist_len, "data", data,
                            TRACE_MRP_DATA_MAX, &data_len) && data_len > 0) {
    ESP_LOGI(TAG, "%s embedded data=%uB", label, (unsigned)data_len);
    if (!trace_mrp_blob(label, data, data_len)) hex_prefix(label, data, data_len);
  }
  heap_caps_free(data);
}

void rtsp_protocol_trace_features(void) {
  const uint64_t features = AIRPLAY_FEATURES;
  const uint64_t fex = AIRPLAY_FEATURES_EX;
  char bits[384] = {0};
  size_t p = 0;
  for (unsigned bit = 0; bit < 64U; ++bit) {
    if ((features & (1ULL << bit)) == 0) continue;
    const int w = snprintf(bits + p, sizeof(bits) - p, "%s%u", p ? "," : "", bit);
    if (w <= 0 || (size_t)w >= sizeof(bits) - p) break;
    p += (size_t)w;
  }
  for (unsigned rel = 0; rel < 64U; ++rel) {
    if ((fex & (1ULL << rel)) == 0) continue;
    const int w = snprintf(bits + p, sizeof(bits) - p, "%s%u", p ? "," : "", rel + 64U);
    if (w <= 0 || (size_t)w >= sizeof(bits) - p) break;
    p += (size_t)w;
  }
  ESP_LOGI(TAG,
           "PTRACE feature-map features=0x%016" PRIx64 " fex=0x%016" PRIx64
           " set-bits=[%s]",
           features, fex, bits);
}

void rtsp_protocol_trace_command(const uint8_t *plist, size_t plist_len) {
  if (!plist || plist_len < 8U || memcmp(plist, "bplist00", 8) != 0) return;
  char type[128] = {0};
  if (bplist_find_string(plist, plist_len, "type", type, sizeof(type))) {
    ESP_LOGI(TAG, "PTRACE /command type=\"%s\" body=%uB", type,
             (unsigned)plist_len);
  } else {
    ESP_LOGI(TAG, "PTRACE /command type=<missing> body=%uB", (unsigned)plist_len);
  }
  if (strcmp(type, "updateMRSupportedCommands") == 0) {
#ifdef CONFIG_AIRPLAY_PROTOCOL_TRACE_MR_COMMANDS
    trace_mr_supported_commands(plist, plist_len);
#endif
  } else if (strcmp(type, "updateMRNowPlayingInfo") == 0) {
    trace_now_playing_info(plist, plist_len);
  }
  trace_bplist_embedded_data("/command", plist, plist_len);
}

void rtsp_protocol_trace_datastream_payload(const char *label,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            size_t full_payload_len) {
  if (!payload || payload_len == 0) return;
  if (payload_len >= 8U && memcmp(payload, "bplist00", 8) == 0) {
    if (payload_len == full_payload_len) {
      trace_bplist_embedded_data(label ? label : "DataStream", payload,
                                 payload_len);
    } else {
      ESP_LOGI(TAG, "%s bplist payload truncated for trace (%u/%uB)",
               label ? label : "DataStream", (unsigned)payload_len,
               (unsigned)full_payload_len);
    }
  } else {
    if (!trace_mrp_blob(label ? label : "DataStream", payload, payload_len)) {
      hex_prefix(label ? label : "DataStream", payload, payload_len);
    }
  }
}

#else

void rtsp_protocol_trace_features(void) {}
void rtsp_protocol_trace_command(const uint8_t *plist, size_t plist_len) {
  (void)plist; (void)plist_len;
}
void rtsp_protocol_trace_datastream_payload(const char *label,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            size_t full_payload_len) {
  (void)label; (void)payload; (void)payload_len; (void)full_payload_len;
}

#endif
