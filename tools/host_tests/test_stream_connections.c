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
      sc.has_rtp || sc.has_rtcp || !sc.has_apap ||
      !sc.apap_use_stream_encryption_key || !sc.has_media_data_control ||
      !sc.has_media_data_control_seed ||
      sc.media_data_control_seed != (uint64_t)(int64_t)-3431997079003895594LL) {
    free(request);
    return 5;
  }
  free(request);

  uint8_t response[768];
  size_t response_len = bplist_build_stream_setup(
      response, sizeof(response), 103, 46666, 40000, 262144,
      123456789U, true, false, false,
      sc.has_apap, 46666, sc.has_media_data_control, 45555,
      sc.media_data_control_seed);
  if (!response_len || write_file(argv[2], response, response_len) != 0) return 6;

  uint8_t ds_response[256];
  size_t ds_len = bplist_build_datastream_setup(
      ds_response, sizeof(ds_response), 45678, 1, true);
  if (!ds_len || write_file(argv[3], ds_response, ds_len) != 0) return 7;

  {
    static const char *const keys[] = {
        "rate", "networkTimeSecs", "networkTimeTimelineID",
        "mediaTimeValue", "mediaTimeScale"};
    const uint64_t values[] = {
        1, 118777, 0xa851abcabebd0008ULL, 39431082691783ULL, 1000000000ULL};
    uint8_t anchor[384];
    const size_t anchor_len = bplist_build_int_dict(
        anchor, sizeof(anchor), keys, values, sizeof(keys) / sizeof(keys[0]));
    int64_t media = 0, timeline = 0, scale = 0;
    if (!anchor_len ||
        !bplist_find_int(anchor, anchor_len, "mediaTimeValue", &media) ||
        media != 39431082691783LL ||
        !bplist_find_int(anchor, anchor_len, "mediaTimeScale", &scale) ||
        scale != 1000000000LL ||
        !bplist_find_int(anchor, anchor_len, "networkTimeTimelineID", &timeline) ||
        (uint64_t)timeline != 0xa851abcabebd0008ULL) {
      return 8;
    }
  }

  return 0;
}
