#include <string.h>

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

