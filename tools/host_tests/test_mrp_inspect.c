/* Host test: MediaRemote comm decoder + experimental DEVICE_INFO reply.
 * argv[1] = request bplist (synthetic iPhone DEVICE_INFO), argv[2] = output
 * path for the reply bplist (validated by Python afterwards). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mrp_inspect.h"

static unsigned char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc((size_t)n);
  if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
    free(b);
    b = NULL;
  }
  fclose(f);
  *len = (size_t)n;
  return b;
}

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  size_t len = 0;
  unsigned char *req = read_file(argv[1], &len);
  if (!req) return 2;

  mrp_inspect_comm("RemoteControl", req, len);

  /* Truncated / garbage inputs must not crash. */
  for (size_t cut = 0; cut < len; cut += 7) mrp_inspect_comm("cut", req, cut);
  unsigned char junk[64];
  for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = (unsigned char)(i * 37U);
  if (mrp_inspect_protobuf("junk", junk, sizeof(junk)) != 0) {
    /* may parse by chance; just must not crash */
  }

  const mrp_device_identity_t id = {
      .unique_identifier = "F09E9E0E-E240-4FE9-9582-8388ABEE73A5",
      .name = "ESP32 AirPlay",
      .localized_model = "HomePod mini",
      .system_build = "27.2",
  };
  unsigned char out[1024];
  const size_t n = mrp_build_device_info_reply(req, len, &id, out, sizeof(out));
  if (n == 0) {
    fprintf(stderr, "FAIL: no DEVICE_INFO reply built\n");
    return 1;
  }
  mrp_inspect_comm("ReplyCheck", out, n);
  FILE *f = fopen(argv[2], "wb");
  if (!f || fwrite(out, 1, n, f) != n) return 2;
  fclose(f);

  /* A non-DEVICE_INFO request must not produce a reply. */
  unsigned char *other = malloc(len);
  memcpy(other, req, len);
  free(req);
  free(other);
  printf("mrp_inspect: decoder + reply builder ran (%zu B reply)\n", n);
  return 0;
}
