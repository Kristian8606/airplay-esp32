/* Host test for bplist_build_update_info(): writes the event-channel
 * updateInfo plist; run.sh decodes it with Python plistlib and checks the
 * structure Shairport sends: { type = "updateInfo"; value = /info + txtAirPlay }.
 * A 700-byte TXT blob exercises the 2-byte bplist length encoding. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plist.h"

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  uint8_t pk[32];
  for (int i = 0; i < 32; i++) pk[i] = (uint8_t)(i * 7 + 1);
  const uint64_t features = ((uint64_t)0x1C340 << 32) | 0x405C4A00;
  static uint8_t out[4096];
  uint8_t txt[512];
  size_t tl = 0;
  const char *items[] = {"deviceid=F0:9E:9E:0E:E2:40", "features=0x405C4A00,0x1C340",
                         "flags=0x4", "model=AudioAccessory5,1", "vv=2", "acl=0"};
  for (unsigned i = 0; i < sizeof items / sizeof *items; i++) {
    const size_t l = strlen(items[i]);
    txt[tl++] = (uint8_t)l;
    memcpy(txt + tl, items[i], l);
    tl += l;
  }
  size_t n = bplist_build_update_info(out, 2048, "F0:9E:9E:0E:E2:40", "Kitchen ESP",
                                      "AudioAccessory5,1", pk, 32, features, 2, txt, tl);
  if (!n) return 3;
  FILE *f = fopen(argv[1], "wb");
  fwrite(out, 1, n, f);
  fclose(f);
  uint8_t big[700];
  memset(big, 'x', sizeof big);
  n = bplist_build_update_info(out, 2048, "F0:9E:9E:0E:E2:40", "Kitchen ESP",
                               "AudioAccessory5,1", pk, 32, features, 2, big, sizeof big);
  if (!n) return 4;
  f = fopen(argv[2], "wb");
  fwrite(out, 1, n, f);
  fclose(f);
  /* Too small a buffer must fail cleanly, never overflow. */
  for (size_t cap = 1024; cap < 1400; cap += 7)
    if (bplist_build_update_info(out, cap, "F0:9E:9E:0E:E2:40", "Kitchen ESP",
                                 "AudioAccessory5,1", pk, 32, features, 2, big, sizeof big) > cap)
      return 5;
  return 0;
}
