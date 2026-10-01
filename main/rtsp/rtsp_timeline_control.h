#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * AirPlay timeline-control model.
 *
 * FLUSHBUFFERED and rate/anchor control describe which media timeline is valid
 * and how RTP or APAP media time maps onto presentation time. This module only parses those
 * messages; it intentionally knows nothing about SETPEERSX, streamConnections,
 * pairing or dynamic stream IDs.
 */
typedef struct {
  bool have_from_seq;
  int64_t from_seq;
  int64_t from_ts;
  bool have_until_seq;
  int64_t until_seq;
  int64_t until_ts;

  bool have_from_media_time;
  int64_t from_media_time_value;
  int64_t from_media_time_scale;
  bool have_until_media_time;
  int64_t until_media_time_value;
  int64_t until_media_time_scale;
} rtsp_flushbuffered_t;

typedef struct {
  bool have_rate;
  double rate;

  uint64_t clock_id;
  bool have_network_time_secs;
  uint64_t network_time_secs;
  uint64_t network_time_frac;
  uint64_t rtp_time;
} rtsp_rate_anchor_t;

/* False means absent/non-bplist body; matching existing handlers this is not
 * itself an RTSP error. */
bool rtsp_timeline_parse_flushbuffered(const uint8_t *plist, size_t plist_len,
                                       rtsp_flushbuffered_t *out);

bool rtsp_timeline_parse_rate_anchor(const uint8_t *plist, size_t plist_len,
                                     rtsp_rate_anchor_t *out);

/* Convert Apple's 32.32 fractional network time into nanoseconds using the
 * same arithmetic as the pre-refactor handler. */
uint64_t rtsp_timeline_network_time_ns(const rtsp_rate_anchor_t *anchor);
