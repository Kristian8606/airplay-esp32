#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apap_frame_queue.h"

int main(void) {
  apap_frame_queue_t *q = NULL;
  assert(apap_frame_queue_create(&q) == ESP_OK);

  const uint8_t aac1[] = {1,2,3,4,5};
  apap_frame_meta_t m1 = {
      .seq = 0x123456,
      .media_sample = 0x123456789ULL,
      .media_time = 4886718345LL,
      .timescale = 44100,
      .aac_len = sizeof(aac1),
  };
  assert(apap_frame_queue_push(q, &m1, aac1, sizeof(aac1)));

  /* Boundary/control frame: valid semantic APAP frame with no AAC payload. */
  apap_frame_meta_t boundary = {
      .seq = 0x123457,
      .media_sample = m1.media_sample + 1024,
      .media_time = m1.media_time + 1024,
      .timescale = 44100,
      .aac_len = 0,
  };
  assert(apap_frame_queue_push(q, &boundary, NULL, 0));

  apap_frame_queue_stats_t st = {0};
  apap_frame_queue_get_stats(q, &st);
  assert(st.frames == 2 && st.bytes == sizeof(aac1));

  uint8_t out[APAP_FRAME_QUEUE_MAX_AAC] = {0};
  apap_frame_meta_t got = {0};
  assert(apap_frame_queue_pop(q, &got, out, sizeof(out)));
  assert(got.seq == m1.seq && got.media_sample == m1.media_sample);
  assert(got.aac_len == sizeof(aac1));
  assert(memcmp(out, aac1, sizeof(aac1)) == 0);

  memset(out, 0xaa, sizeof(out));
  assert(apap_frame_queue_pop(q, &got, out, sizeof(out)));
  assert(got.seq == boundary.seq && got.media_sample == boundary.media_sample);
  assert(got.aac_len == 0);

  /* Fill to capacity, verify backpressure, then O(1) clear. */
  for (uint32_t i = 0; i < APAP_FRAME_QUEUE_CAPACITY; ++i) {
    apap_frame_meta_t m = {
        .seq = i,
        .media_sample = 1000U + (uint64_t)i * 1024U,
        .media_time = 1000 + (int64_t)i * 1024,
        .timescale = 44100,
    };
    assert(apap_frame_queue_push(q, &m, NULL, 0));
  }
  assert(!apap_frame_queue_push(q, &m1, aac1, sizeof(aac1)));
  apap_frame_queue_clear(q);
  apap_frame_queue_get_stats(q, &st);
  assert(st.frames == 0 && st.bytes == 0);
  assert(st.dropped_on_clear >= APAP_FRAME_QUEUE_CAPACITY);

  apap_frame_queue_destroy(q);
  puts("APAP semantic frame queue: ok");
  return 0;
}
