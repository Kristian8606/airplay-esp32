/* Host test for the buffered FLUSH classifier (v4.1.47), extracted from
 * audio_receiver.c by extract_flush.py. Replays block sequences taken from
 * real HomePod logs. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_err.h"

void t_init(void);
void t_immediate(uint32_t seq, uint32_t ts, bool valid);
esp_err_t t_deferred(uint32_t fs, uint32_t ft, uint32_t us, uint32_t ut);
bool t_armed_ts(void);
bool t_active(void);
int t_classify(uint32_t seq, uint32_t rtp, int hint_valid, int32_t lead, int *end_kind);

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
enum { END_SEQ = 0, END_MARKER = 1, END_TIMELINE = 2 };

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int end;
  t_init();

  /* T1 Shairport semantics unchanged for a valid untilSeq (upper half). */
  t_immediate(15892175, 3564955811u, true);           /* & 0x7fffff = 7503567 */
  CHECK(t_classify(7503560, 1, 0, 0, &end) == 1 && end == -1);
  CHECK(t_classify(7503567, 2, 0, 0, &end) == 0 && end == END_SEQ);
  CHECK(!t_active());
  printf("T1 valid untilSeq: sequential end kept: ok\n");

  /* T2 the old bug: untilSeq=0 with seq in the upper half. Before v4.1.47 the
   * mod-2^23 compare dropped every block until the wrap; now the block whose
   * RTP == untilTS ends it (log: untilTS=3770786987, block 7948529 carries
   * rtp=3770786987 and also ends the deferred ranges). */
  t_init();
  CHECK(t_deferred(16330753, 3770786987u, 16337137, 3774872478u) == ESP_OK);
  t_immediate(0, 3770786987u, false);
  CHECK(t_armed_ts());
  for (uint32_t seq = 7942145; seq < 7948529; ++seq)   /* old blocks */
    CHECK(t_classify(seq, 3770787000u + (seq - 7942145) * 1024u, 0, 0, &end) == 1);
  CHECK(t_classify(7948529, 3770786987u, 0, 0, &end) == 0 && end == END_MARKER);
  CHECK(!t_active() && !t_armed_ts());
  /* Completion cancelled the deferred range, the next block plays. */
  CHECK(t_classify(7948530, 3770788011u, 0, 0, &end) == 0 && end == -1);
  printf("T2 untilSeq=0 ends at the untilTS marker block: ok\n");

  /* T3 marker never comes (log 146 s, v4.1.47): the sender really seeked, the
   * new content has a new RTP base. lead = block RTP - new anchor RTP
   * (anchor rtp=4109557308). Blocks 6972024.. at rtp 4107810038 are 39.6 s
   * before the anchor and are dropped; 6972085 at rtp 4109631950 (+1.7 s)
   * ends the flush. */
  t_init();
  t_immediate(0, 2714667841u, false);
  CHECK(t_classify(6970000, 2718913790u, 0, 0, &end) == 1);        /* no anchor yet */
  CHECK(t_classify(6972024, 4107810038u, 1,
                   (int32_t)(4107810038u - 4109557308u), &end) == 1); /* -39.6 s */
  CHECK(t_classify(6972085, 4109631950u, 1,
                   (int32_t)(4109631950u - 4109557308u), &end) == 0 &&
        end == END_TIMELINE);
  CHECK(!t_active());
  /* Window edges: +/-10 s around the anchor RTP. */
  t_init();
  t_immediate(0, 1u, false);
  CHECK(t_classify(1, 2u, 1, 10 * 44100 + 1, &end) == 1);
  CHECK(t_classify(2, 3u, 1, -10 * 44100 - 1, &end) == 1);
  CHECK(t_classify(3, 4u, 1, 5 * 44100, &end) == 0 && end == END_TIMELINE);
  printf("T3 untilSeq=0 without marker ends on the new anchor timeline: ok\n");

  /* T4 a proper immediate FLUSH arriving 260 ms later replaces the seq-0 one
   * (log 185.7 s -> 185.97 s) and uses the sequential rule again. */
  t_init();
  t_immediate(0, 3770786987u, false);
  t_immediate(16337270, 834458953u, true);                /* 7948662 */
  CHECK(!t_armed_ts());
  CHECK(t_classify(7948600, 3770786987u, 1, 0, &end) == 1);  /* marker ignored */
  CHECK(t_classify(7948662, 1u, 0, 0, &end) == 0 && end == END_SEQ);
  printf("T4 a later valid immediate FLUSH replaces the seq-0 one: ok\n");

  /* T5 lower-half sessions behave as before (0 would have ended at once);
   * now it ends at the marker / timeline, never later than the new audio. */
  t_init();
  t_immediate(0, 42u, false);
  CHECK(t_classify(4099067, 41u, 1, 500, &end) == 0 && end == END_TIMELINE);
  printf("T5 lower-half sequence numbers: ok\n");
  printf("flush classifier: all tests passed\n");
  return 0;
}
