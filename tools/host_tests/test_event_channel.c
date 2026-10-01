/* Host test for the AirPlay 2 event channel crypto (v4.1.46).
 *
 * Uses the real hap_derive_event_keys() (hap_crypto.c) and the real
 * rtsp_crypto_seal_send() / rtsp_crypto_open() (rtsp_crypto.c) with libsodium.
 * test_event_channel.py plays the sender with an independent implementation
 * (Python "cryptography": HKDF-SHA512 + ChaCha20-Poly1305) and the key
 * orientation of Shairport Sync's server-side cipher channel 4:
 *   receiver encrypts with "Events-Write-Encryption-Key",
 *   receiver decrypts with "Events-Read-Encryption-Key".
 *
 *   test_event_channel seal <orientation 1|0> <out file>
 *   test_event_channel open <orientation 1|0> <in file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hap.h"
#include "rtsp_crypto.h"

static const uint8_t IKM[32] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
    0xcc, 0xdd, 0xee, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};

/* Message longer than one 0x400 block so framing and nonce stepping are
 * exercised. */
static size_t build_message(uint8_t *out, size_t cap) {
  size_t n = (size_t)snprintf((char *)out, cap,
                              "POST /command RTSP/1.0\r\nContent-Length: 2500\r\n"
                              "Content-Type: application/x-apple-binary-plist\r\n\r\n");
  for (size_t i = 0; i < 2500 && n < cap; ++i) out[n++] = (uint8_t)(i * 31u + 7u);
  return n;
}

int main(int argc, char **argv) {
  if (argc != 4) return 2;
  hap_session_t s;
  memset(&s, 0, sizeof(s));

  if (strcmp(argv[1], "datakeys") == 0) {
    memcpy(s.shared_secret, IKM, sizeof(IKM));
    s.session_established = true;
    const uint64_t seed = strtoull(argv[2], NULL, 10);
    uint8_t enc[32], dec[32];
    if (hap_derive_datastream_keys(&s, seed, enc, dec) != ESP_OK) return 11;
    FILE *f = fopen(argv[3], "wb");
    if (!f) return 12;
    if (fwrite(enc, 1, sizeof(enc), f) != sizeof(enc) ||
        fwrite(dec, 1, sizeof(dec), f) != sizeof(dec)) {
      fclose(f);
      return 13;
    }
    fclose(f);
    return 0;
  }

  hap_derive_event_keys(&s, IKM, sizeof(IKM), argv[2][0] == '1');
  if (!s.event_keys_valid) return 3;

  if (strcmp(argv[1], "seal") == 0) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 4;
    static uint8_t msg[4096];
    const size_t len = build_message(msg, sizeof(msg));
    uint64_t nonce = 0;
    if (rtsp_crypto_seal_send(sv[0], s.event_encrypt_key, &nonce, msg, len) != 0)
      return 5;
    close(sv[0]);
    FILE *f = fopen(argv[3], "wb");
    uint8_t buf[1024];
    ssize_t r;
    while ((r = read(sv[1], buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)r, f);
    fclose(f);
    printf("sealed %zu bytes in %llu frames\n", len, (unsigned long long)nonce);
    return nonce == 3 ? 0 : 6;
  }

  /* open: every frame in the file must authenticate with the decrypt key. */
  FILE *f = fopen(argv[3], "rb");
  if (!f) return 7;
  static uint8_t in[8192];
  const size_t n = fread(in, 1, sizeof(in), f);
  fclose(f);
  static uint8_t plain[RTSP_ENCRYPTED_BLOCK_MAX];
  uint64_t nonce = 0;
  size_t pos = 0;
  while (pos + 2 <= n) {
    const size_t blen = (size_t)in[pos] | ((size_t)in[pos + 1] << 8);
    const int pl = rtsp_crypto_open(s.event_decrypt_key, &nonce, in + pos,
                                    2 + blen + 16, plain);
    if (pl < 0) {
      printf("open: authentication failed at frame %llu\n",
             (unsigned long long)nonce);
      return 8;
    }
    printf("open: frame %llu: %.*s\n", (unsigned long long)nonce - 1,
           pl > 40 ? 40 : pl, (const char *)plain);
    pos += 2 + blen + 16;
  }
  /* A tampered frame must be rejected and must not advance the nonce. */
  in[2] ^= 1;
  uint64_t n0 = 0;
  if (rtsp_crypto_open(s.event_decrypt_key, &n0, in, n, plain) >= 0 || n0 != 0)
    return 9;
  return (pos == n && nonce > 0) ? 0 : 10;
}
