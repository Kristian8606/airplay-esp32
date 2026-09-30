#include <string.h>
#include <stdio.h>

#include "hap.h"
#include "hap_internal.h"

#include "esp_log.h"
#include "sodium.h"

static const char *TAG = "hap_crypto";

int hap_hkdf_sha512(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                    size_t ikm_len, const uint8_t *info, size_t info_len,
                    uint8_t *okm, size_t okm_len) {
  uint8_t prk[crypto_auth_hmacsha512_BYTES];
  crypto_auth_hmacsha512_state state;

  if (salt && salt_len > 0) {
    crypto_auth_hmacsha512_init(&state, salt, salt_len);
  } else {
    uint8_t zero_salt[crypto_auth_hmacsha512_BYTES] = {0};
    crypto_auth_hmacsha512_init(&state, zero_salt, sizeof(zero_salt));
  }
  crypto_auth_hmacsha512_update(&state, ikm, ikm_len);
  crypto_auth_hmacsha512_final(&state, prk);

  uint8_t t[crypto_auth_hmacsha512_BYTES];
  uint8_t counter = 1;
  size_t t_len = 0;
  size_t pos = 0;

  while (pos < okm_len) {
    crypto_auth_hmacsha512_init(&state, prk, sizeof(prk));
    if (t_len > 0) {
      crypto_auth_hmacsha512_update(&state, t, t_len);
    }
    if (info && info_len > 0) {
      crypto_auth_hmacsha512_update(&state, info, info_len);
    }
    crypto_auth_hmacsha512_update(&state, &counter, 1);
    crypto_auth_hmacsha512_final(&state, t);
    t_len = crypto_auth_hmacsha512_BYTES;

    size_t copy_len = okm_len - pos;
    if (copy_len > crypto_auth_hmacsha512_BYTES) {
      copy_len = crypto_auth_hmacsha512_BYTES;
    }
    memcpy(okm + pos, t, copy_len);
    pos += copy_len;
    counter++;
  }

  sodium_memzero(prk, sizeof(prk));
  sodium_memzero(t, sizeof(t));
  return 0;
}

void hap_derive_event_keys(hap_session_t *session, const uint8_t *ikm,
                           size_t ikm_len, bool control_encrypt_is_read) {
  if (!session || !ikm || ikm_len == 0) {
    return;
  }
  static const char salt[] = "Events-Salt";
  static const char write_info[] = "Events-Write-Encryption-Key";
  static const char read_info[] = "Events-Read-Encryption-Key";
  const char *enc_info = control_encrypt_is_read ? write_info : read_info;
  const char *dec_info = control_encrypt_is_read ? read_info : write_info;
  hap_hkdf_sha512((const uint8_t *)salt, sizeof(salt) - 1, ikm, ikm_len,
                  (const uint8_t *)enc_info, strlen(enc_info),
                  session->event_encrypt_key, sizeof(session->event_encrypt_key));
  hap_hkdf_sha512((const uint8_t *)salt, sizeof(salt) - 1, ikm, ikm_len,
                  (const uint8_t *)dec_info, strlen(dec_info),
                  session->event_decrypt_key, sizeof(session->event_decrypt_key));
  session->event_keys_valid = true;
}

esp_err_t hap_derive_audio_key(hap_session_t *session, uint8_t *audio_key,
                               size_t key_len) {
  if (!session || !audio_key || key_len < 16) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!session->session_established) {
    ESP_LOGW(TAG, "Cannot derive audio key before session established");
    return ESP_ERR_INVALID_STATE;
  }

  hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, session->shared_secret, 32,
                  (uint8_t *)"Control-Read-Encryption-Key", 27, audio_key,
                  key_len);

  return ESP_OK;
}


esp_err_t hap_derive_datastream_keys(const hap_session_t *session, uint64_t seed,
                                     uint8_t encrypt_key[HAP_CHACHA20_KEY_SIZE],
                                     uint8_t decrypt_key[HAP_CHACHA20_KEY_SIZE]) {
  if (!session || !encrypt_key || !decrypt_key) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!session->session_established) {
    ESP_LOGW(TAG, "Cannot derive DataStream keys before session established");
    return ESP_ERR_INVALID_STATE;
  }

  char salt[64];
  const int n = snprintf(salt, sizeof(salt), "DataStream-Salt%llu",
                         (unsigned long long)seed);
  if (n <= 0 || (size_t)n >= sizeof(salt)) {
    return ESP_ERR_INVALID_SIZE;
  }

  static const char input_info[] = "DataStream-Input-Encryption-Key";
  static const char output_info[] = "DataStream-Output-Encryption-Key";

  /* The AirPlay sender opens the TCP connection.  Its output key is our
   * decrypt key; its input key is our encrypt key (Shairport cipher channel 5). */
  hap_hkdf_sha512((const uint8_t *)salt, (size_t)n,
                  session->shared_secret, sizeof(session->shared_secret),
                  (const uint8_t *)input_info, sizeof(input_info) - 1,
                  encrypt_key, HAP_CHACHA20_KEY_SIZE);
  hap_hkdf_sha512((const uint8_t *)salt, (size_t)n,
                  session->shared_secret, sizeof(session->shared_secret),
                  (const uint8_t *)output_info, sizeof(output_info) - 1,
                  decrypt_key, HAP_CHACHA20_KEY_SIZE);
  sodium_memzero(salt, sizeof(salt));
  return ESP_OK;
}
