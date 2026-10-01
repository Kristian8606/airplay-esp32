#!/usr/bin/env python3
"""Extract the output-volume code from main/audio/audio_receiver.c into
vol_under_test.c so it can be unit-tested on the host."""
import pathlib, sys
root = pathlib.Path(__file__).resolve().parents[2]
src = (root / "main/audio/audio_receiver.c").read_text()
a = src.index("static uint32_t s_vol_dither_state[2]")
b = src.index("/* I2S lifecycle is single-owner on the playout task.")
out = ("#include <stdint.h>\n#include <string.h>\n#include <stdbool.h>\n"
       "static volatile int32_t s_volume_target_q15 = 32768;\n" + src[a:b] +
       "\nvoid vol_new(int16_t *p, uint32_t f, int32_t *c, int32_t t) {\n"
       "  s_volume_target_q15 = t;\n  apply_output_volume(p, f, c);\n}\n")
pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "vol_under_test.c").write_text(out)
