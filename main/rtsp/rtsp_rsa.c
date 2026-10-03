#include "rtsp_rsa.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "sodium/utils.h" // sodium_base642bin, sodium_bin2base64

static const char *TAG = "rtsp_rsa";

// Well-known AirPlay RSA private key (public knowledge, used by all receivers)
static const char airplay_rsa_private_key[] =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Q\n"
    "g90u3sG/1CUtwC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6\n"
    "DZHJ2YCCLlDRKSKv6kDqnw4UwPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLv\n"
    "qr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf/+sG+NCK3eQJVxqcaJ/vEHKIVd2M\n"
    "+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/UAaHqn9JdsBWLUEp\n"
    "VviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfWBLmk\n"
    "zkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14n\n"
    "DY4TFQAaLlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPsc\n"
    "EsA5ltpxOgUGCY7b7ez5NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZu\n"
    "NGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jmlpPHr0O/KnPQtzI3eguhe0TwUem/eYSd\n"
    "yzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurcizaaA/L0HIgAmOit1GJA2s\n"
    "aMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFua39GLS99\n"
    "ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrn\n"
    "jndMoPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v\n"
    "//mU8eVkQaoANf0ZoMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtk\n"
    "rfa7ef+AUb69DNggq4mHQAYBp7L+k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQ\n"
    "NepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hLAoGBANDrr7xAJbqBjHVwIzQ4\n"
    "To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvAcJyRM9SJ7OKl\n"
    "Gt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
    "54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TF\n"
    "BVmD7fV0Zhov18fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLa\n"
    "lpGSwomSNYJcB9HNMlmhkGzc1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9\n"
    "+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gILAuE4Pu13aKiJnfft7hIjbK+5kyb\n"
    "3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ2gG0N5hvJpzwwhbh\n"
    "XqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
    "-----END RSA PRIVATE KEY-----";

// Serialize key initialization and RSA operations across RTSP clients.
static StaticSemaphore_t s_rsa_mutex_storage;
static SemaphoreHandle_t s_rsa_mutex;
static portMUX_TYPE s_rsa_mutex_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t lock_rsa(void) {
  portENTER_CRITICAL(&s_rsa_mutex_mux);
  if (!s_rsa_mutex) {
    s_rsa_mutex = xSemaphoreCreateMutexStatic(&s_rsa_mutex_storage);
  }
  SemaphoreHandle_t mutex = s_rsa_mutex;
  portEXIT_CRITICAL(&s_rsa_mutex_mux);

  if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
    return NULL;
  }
  return mutex;
}

static bool s_pk_initialized = false;

#define AIRPLAY_RSA_BITS 2048

// Keep separate PSA handles with an explicit algorithm policy for
// the Apple-Challenge signature and the RAOP AES key decryption.
static mbedtls_svc_key_id_t s_sign_key;
static mbedtls_svc_key_id_t s_decrypt_key;

static int ensure_pk_initialized(void) {
  if (s_pk_initialized) {
    return 0;
  }

  psa_status_t status = psa_crypto_init();
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Failed to initialize PSA crypto: %d", (int)status);
    return -1;
  }

  // The embedded PEM contains a PKCS#1 RSAPrivateKey, the binary format
  // accepted by psa_import_key for PSA_KEY_TYPE_RSA_KEY_PAIR.
  const char *body = strchr(airplay_rsa_private_key, '\n');
  const char *footer = strstr(airplay_rsa_private_key,
                              "-----END RSA PRIVATE KEY-----");
  if (!body || !footer) {
    return -1;
  }
  ++body;
  if (footer <= body) {
    return -1;
  }
  size_t der_size = (size_t)(footer - body);
  uint8_t *der = malloc(der_size);
  if (!der) {
    return -1;
  }
  size_t der_len = 0;
  if (sodium_base642bin(der, der_size, body, der_size, "\r\n \t", &der_len,
                        NULL, sodium_base64_VARIANT_ORIGINAL) != 0) {
    ESP_LOGE(TAG, "Failed to decode RSA private key");
    sodium_memzero(der, der_size);
    free(der);
    return -1;
  }

  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
  psa_set_key_bits(&attributes, AIRPLAY_RSA_BITS);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
  psa_set_key_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_SIGN_RAW);
  status = psa_import_key(&attributes, der, der_len, &s_sign_key);
  if (status == PSA_SUCCESS) {
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1));
    status = psa_import_key(&attributes, der, der_len, &s_decrypt_key);
  }
  psa_reset_key_attributes(&attributes);
  sodium_memzero(der, der_size);
  free(der);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Failed to import RSA private key: %d", (int)status);
    psa_destroy_key(s_sign_key);
    psa_destroy_key(s_decrypt_key);
    s_sign_key = 0;
    s_decrypt_key = 0;
    return -1;
  }

  s_pk_initialized = true;
  return 0;
}

// Simple base64 decode using libsodium
static int b64_decode(const char *b64, uint8_t *out, size_t out_size,
                      size_t *out_len) {
  // Apple base64 may lack padding — libsodium handles that with _IGNORE variant
  if (sodium_base642bin(out, out_size, b64, strlen(b64), "\r\n \t", out_len,
                        NULL, sodium_base64_VARIANT_ORIGINAL_NO_PADDING) != 0) {
    // Try with padding variant
    if (sodium_base642bin(out, out_size, b64, strlen(b64), "\r\n \t", out_len,
                          NULL, sodium_base64_VARIANT_ORIGINAL) != 0) {
      return -1;
    }
  }
  return 0;
}

// Simple base64 encode using libsodium, strip trailing '='
static int b64_encode(const uint8_t *data, size_t data_len, char *out,
                      size_t out_size) {
  char *result = sodium_bin2base64(out, out_size, data, data_len,
                                   sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
  return result ? 0 : -1;
}

static int apple_challenge_response_locked(const char *challenge_b64, uint32_t ip_addr,
                                 const uint8_t mac[6], char *out_b64,
                                 size_t out_b64_size) {
  if (ensure_pk_initialized() != 0) {
    return -1;
  }

  // Decode challenge
  uint8_t challenge[32];
  size_t challenge_len = 0;
  if (b64_decode(challenge_b64, challenge, sizeof(challenge), &challenge_len) !=
      0) {
    ESP_LOGE(TAG, "Failed to decode Apple-Challenge");
    return -1;
  }

  // Build response data: challenge + IP(4) + MAC(6), padded to 32 bytes
  uint8_t data[32];
  memset(data, 0, sizeof(data));
  size_t pos = 0;

  if (challenge_len > 22) {
    challenge_len = 22; // Max room for challenge if IP + MAC must fit
  }
  memcpy(data + pos, challenge, challenge_len);
  pos += challenge_len;

  memcpy(data + pos, &ip_addr, 4);
  pos += 4;

  memcpy(data + pos, mac, 6);
  // pos is now at most 32, rest is zero-padded

  // RAW applies type-1 padding directly to the 32-byte response data, without
  // hashing or adding DigestInfo, matching RSA_private_encrypt in RAOP.
  size_t rsa_len = PSA_SIGN_OUTPUT_SIZE(PSA_KEY_TYPE_RSA_KEY_PAIR,
                                       AIRPLAY_RSA_BITS,
                                       PSA_ALG_RSA_PKCS1V15_SIGN_RAW);
  uint8_t *rsa_out = malloc(rsa_len);
  if (!rsa_out) {
    return -1;
  }
  size_t signature_len = 0;
  psa_status_t status = psa_sign_hash(s_sign_key,
                                      PSA_ALG_RSA_PKCS1V15_SIGN_RAW,
                                      data, sizeof(data), rsa_out, rsa_len,
                                      &signature_len);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "RSA sign failed: %d", (int)status);
    free(rsa_out);
    return -1;
  }
  rsa_len = signature_len;
  int ret;

  // Base64 encode result without padding
  ret = b64_encode(rsa_out, rsa_len, out_b64, out_b64_size);
  free(rsa_out);

  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to base64-encode RSA response");
    return -1;
  }

  return 0;
}

static int decrypt_aes_key_locked(const char *encrypted_b64, uint8_t *out_key,
                        size_t out_key_size, size_t *out_key_len) {
  if (ensure_pk_initialized() != 0) {
    return -1;
  }

  // Decode the base64 RSA-encrypted key
  uint8_t encrypted[512];
  size_t encrypted_len = 0;
  if (b64_decode(encrypted_b64, encrypted, sizeof(encrypted), &encrypted_len) !=
      0) {
    ESP_LOGE(TAG, "Failed to decode RSA-encrypted AES key");
    return -1;
  }

  ESP_LOGI(TAG, "RSA-encrypted AES key: %zu bytes (expected 256)",
           encrypted_len);

  // RSA OAEP-SHA1 decrypt (RAOP uses OAEP padding for the AES key).
  size_t olen = 0;
  psa_status_t status = psa_asymmetric_decrypt(
      s_decrypt_key, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), encrypted, encrypted_len,
      NULL, 0, out_key, out_key_size, &olen);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "RSA AES key decrypt failed: %d", (int)status);
    return -1;
  }

  *out_key_len = olen;
  ESP_LOGI(TAG, "Decrypted AES key: %zu bytes", olen);
  return 0;
}

int rsa_apple_challenge_response(const char *challenge_b64, uint32_t ip_addr,
                                 const uint8_t mac[6], char *out_b64,
                                 size_t out_b64_size) {
  SemaphoreHandle_t mutex = lock_rsa();
  if (!mutex) {
    return -1;
  }
  int ret = apple_challenge_response_locked(challenge_b64, ip_addr, mac,
                                             out_b64, out_b64_size);
  xSemaphoreGive(mutex);
  return ret;
}

int rsa_decrypt_aes_key(const char *encrypted_b64, uint8_t *out_key,
                        size_t out_key_size, size_t *out_key_len) {
  SemaphoreHandle_t mutex = lock_rsa();
  if (!mutex) {
    return -1;
  }
  int ret = decrypt_aes_key_locked(encrypted_b64, out_key, out_key_size,
                                   out_key_len);
  xSemaphoreGive(mutex);
  return ret;
}
