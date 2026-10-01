#include "rtsp_timeline_control.h"

#include <string.h>

#include "plist.h"

static bool is_bplist(const uint8_t *plist, size_t plist_len) {
  return plist && plist_len >= 8 && memcmp(plist, "bplist00", 8) == 0;
}

bool rtsp_timeline_parse_flushbuffered(const uint8_t *plist, size_t plist_len,
                                       rtsp_flushbuffered_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!is_bplist(plist, plist_len)) return false;

  out->have_from_seq =
      bplist_find_int(plist, plist_len, "flushFromSeq", &out->from_seq);
  (void)bplist_find_int(plist, plist_len, "flushFromTS", &out->from_ts);
  out->have_until_seq =
      bplist_find_int(plist, plist_len, "flushUntilSeq", &out->until_seq);
  (void)bplist_find_int(plist, plist_len, "flushUntilTS", &out->until_ts);
  out->have_from_media_time =
      bplist_find_int(plist, plist_len, "flushFromMediaTimeValue",
                      &out->from_media_time_value);
  (void)bplist_find_int(plist, plist_len, "flushFromMediaTimeScale",
                        &out->from_media_time_scale);
  out->have_until_media_time =
      bplist_find_int(plist, plist_len, "flushUntilMediaTimeValue",
                      &out->until_media_time_value);
  (void)bplist_find_int(plist, plist_len, "flushUntilMediaTimeScale",
                        &out->until_media_time_scale);
  return true;
}

bool rtsp_timeline_parse_rate_anchor(const uint8_t *plist, size_t plist_len,
                                     rtsp_rate_anchor_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->rate = 1.0;
  if (!is_bplist(plist, plist_len)) return false;

  if (bplist_find_real(plist, plist_len, "rate", &out->rate)) {
    out->have_rate = true;
  } else {
    int64_t rate_int = 0;
    if (bplist_find_int(plist, plist_len, "rate", &rate_int)) {
      out->rate = (double)rate_int;
      out->have_rate = true;
    }
  }

  int64_t value = 0;
  if (bplist_find_int(plist, plist_len, "networkTimeTimelineID", &value)) {
    out->clock_id = (uint64_t)value;
  }
  if (bplist_find_int(plist, plist_len, "networkTimeSecs", &value)) {
    out->network_time_secs = (uint64_t)value;
    out->have_network_time_secs = true;
  }
  if (bplist_find_int(plist, plist_len, "networkTimeFrac", &value)) {
    out->network_time_frac = (uint64_t)value;
  }
  if (bplist_find_int(plist, plist_len, "rtpTime", &value)) {
    out->rtp_time = (uint64_t)value;
  }

  return true;
}

uint64_t rtsp_timeline_network_time_ns(const rtsp_rate_anchor_t *anchor) {
  if (!anchor) return 0;
  uint64_t frac = anchor->network_time_frac >> 32;
  frac = (frac * 1000000000ULL) >> 32;
  return anchor->network_time_secs * 1000000000ULL + frac;
}
