#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "plist.h"

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long n = ftell(f);
  if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  uint8_t *buf = malloc((size_t)n);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf); fclose(f); return NULL;
  }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  if (fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
  fclose(f);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 4) return 2;

  size_t request_len = 0;
  uint8_t *request = read_file(argv[1], &request_len);
  if (!request) return 3;

  bool loudness = false;
  if (!bplist_find_bool(request, request_len,
                        "loudnessNormalizationEnabled", &loudness) ||
      !loudness) {
    free(request);
    return 4;
  }

  bplist_stream_connection_info_t sc = {0};
  if (!bplist_get_stream_connection_info(request, request_len, 0, &sc) ||
      !sc.has_rtp || !sc.has_rtcp || !sc.has_media_data_control ||
      !sc.has_media_data_control_seed ||
      sc.media_data_control_seed != (uint64_t)(int64_t)-3431997079003895594LL) {
    free(request);
    return 5;
  }
  free(request);

  uint8_t response[768];
  size_t response_len = bplist_build_stream_setup(
      response, sizeof(response), 103, 58911, 40000, 6291456,
      123456789U, true, sc.has_rtp, sc.has_rtcp,
      sc.has_media_data_control, 45555, sc.media_data_control_seed);
  if (!response_len || write_file(argv[2], response, response_len) != 0) return 6;

  uint8_t ds_response[256];
  size_t ds_len = bplist_build_datastream_setup(
      ds_response, sizeof(ds_response), 45678, 1, true);
  if (!ds_len || write_file(argv[3], ds_response, ds_len) != 0) return 7;

  return 0;
}
