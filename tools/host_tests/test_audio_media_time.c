#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include "audio_media_time.h"

int main(void) {
  uint64_t sample = 0;

  assert(audio_media_time_to_sample64(28019334008224LL, 1000000000U,
                                      44100U, &sample));
  assert(sample == 1235652629ULL);

  assert(audio_media_time_to_sample64(70758001719561LL, 1000000000U,
                                      44100U, &sample));
  assert(sample == 3120427875ULL);

  assert(audio_media_time_to_sample64(3376032785LL, 44100U,
                                      44100U, &sample));
  assert(sample == 3376032785ULL);

  assert(!audio_media_time_to_sample64(-1, 44100U, 44100U, &sample));
  assert(!audio_media_time_to_sample64(1, 0U, 44100U, &sample));

  puts("audio media-time conversion: ok");
  return 0;
}
