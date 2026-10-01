#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plist.h"

int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb");
  assert(f);
  assert(fseek(f, 0, SEEK_END) == 0);
  long n = ftell(f);
  assert(n > 0);
  rewind(f);
  uint8_t *buf = malloc((size_t)n);
  assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
  fclose(f);

  char text[64] = {0};
  double real = 0.0;
  int64_t integer = 0;
  bool boolean = true;
  size_t data_len = 0;

  assert(bplist_find_string_deep(buf, (size_t)n,
                                 "kMRMediaRemoteNowPlayingInfoTitle",
                                 text, sizeof(text)));
  assert(strcmp(text, "Song") == 0);
  assert(bplist_find_real_deep(buf, (size_t)n,
                               "kMRMediaRemoteNowPlayingInfoElapsedTime",
                               &real));
  assert(fabs(real - 31.879) < 0.001);
  assert(bplist_find_int_deep(buf, (size_t)n,
                              "kMRMediaRemoteNowPlayingInfoQueueIndex",
                              &integer));
  assert(integer == 18);
  assert(bplist_find_bool_deep(buf, (size_t)n,
                               "kMRMediaRemoteNowPlayingInfoIsInTransition",
                               &boolean));
  assert(boolean == false);
  assert(bplist_find_data_len_deep(buf, (size_t)n,
                                   "kMRMediaRemoteNowPlayingInfoArtworkData",
                                   &data_len));
  assert(data_len == 135106U);

  free(buf);
  puts("nested MediaRemote metadata: ok");
  return 0;
}
