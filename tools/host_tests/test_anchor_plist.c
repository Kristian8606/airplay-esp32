/* Host test for bplist_build_anchor() (GETANCHOR reply): writes the plist;
 * run.sh decodes it with Python plistlib and checks keys and values, incl.
 * 64-bit values with the top bit set (sender-style negative frac/clock id). */
#include <stdint.h>
#include <stdio.h>

#include "plist.h"

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  static uint8_t out[256];
  const size_t n = bplist_build_anchor(out, sizeof(out), 1, 3778452185ULL,
                                       16670ULL, 0xA455AE1CE0000000ULL, 0,
                                       0xC4168F40983F0008ULL);
  if (!n) return 3;
  if (bplist_build_anchor(out, 40, 1, 1, 1, 1, 0, 1) != 0) return 4;
  FILE *f = fopen(argv[1], "wb");
  fwrite(out, 1, n, f);
  fclose(f);
  char text[256];
  bplist_describe(out, n, text, sizeof(text));
  printf("anchor plist: %s\n", text);
  return 0;
}
