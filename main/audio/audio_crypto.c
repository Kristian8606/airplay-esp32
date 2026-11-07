#include "audio_crypto.h"

#include "ap2_audio_crypto.h"

/* The stream key for ap2_audio_decrypt_*(): NULL copies the payload. */
static const uint8_t *stream_key(const audio_encrypt_t *encrypt) {
  return encrypt && encrypt->type == AUDIO_ENCRYPT_CHACHA20_POLY1305
             ? encrypt->key
             : NULL;
}

int audio_crypto_decrypt_rtp(const audio_encrypt_t *encrypt,
                             const uint8_t *input, size_t input_len,
                             uint8_t *output, size_t output_capacity,
                             const uint8_t *full_packet,
                             size_t full_packet_len) {
  if (!encrypt || !input || !output) return -1;
  return ap2_audio_decrypt_rtp(stream_key(encrypt), input, input_len, output,
                               output_capacity, full_packet, full_packet_len);
}

int audio_crypto_decrypt_buffered(const audio_encrypt_t *encrypt,
                                  const uint8_t *packet, size_t packet_len,
                                  uint8_t *output, size_t output_capacity) {
  if (!packet || !output) return -1;
  return ap2_audio_decrypt_buffered(stream_key(encrypt), packet, packet_len,
                                    output, output_capacity);
}
