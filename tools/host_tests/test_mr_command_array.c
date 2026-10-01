#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "plist.h"

static uint8_t *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  assert(f);
  assert(fseek(f, 0, SEEK_END) == 0);
  long n = ftell(f);
  assert(n > 0);
  rewind(f);
  uint8_t *buf = malloc((size_t)n);
  assert(buf);
  assert(fread(buf, 1, (size_t)n, f) == (size_t)n);
  fclose(f);
  *out_len = (size_t)n;
  return buf;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  size_t outer_len = 0;
  uint8_t *outer = read_file(argv[1], &outer_len);

  const int expected_cmds[] = {0, 24, 25};
  const bool expected_enabled[] = {true, true, false};
  uint8_t inner[1024];

  for (size_t i = 0; i < 3; ++i) {
    size_t inner_len = 0;
    size_t count = 0;
    assert(bplist_get_data_array_item_deep(
        outer, outer_len, "mrSupportedCommandsFromSender", i, inner,
        sizeof(inner), &inner_len, &count));
    assert(count == 3);
    assert(inner_len > 8);

    int64_t cmd = -1;
    bool enabled = false;
    assert(bplist_find_int(inner, inner_len, "kCommandInfoCommandKey", &cmd));
    assert(bplist_find_bool(inner, inner_len, "kCommandInfoEnabledKey",
                            &enabled));
    assert(cmd == expected_cmds[i]);
    assert(enabled == expected_enabled[i]);
  }

  size_t dummy_len = 123;
  size_t count = 0;
  assert(!bplist_get_data_array_item_deep(
      outer, outer_len, "mrSupportedCommandsFromSender", 3, inner,
      sizeof(inner), &dummy_len, &count));
  assert(count == 3);
  assert(dummy_len == 0);

  free(outer);
  puts("MR supported-command DATA array: ok");
  return 0;
}
