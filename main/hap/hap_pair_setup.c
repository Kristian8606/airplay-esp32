#include <string.h>

#include "airplay_identity.h"
#include "hap.h"
#include "hap_internal.h"
#include "hap_pairings.h"
#include "srp.h"
#include "tlv8.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "sodium.h"

static const char *TAG = "hap_setup";

#define TLV_TYPE_METHOD 0x00
#define TLV_TYPE_SALT   0x02
#define TLV_TYPE_PROOF  0x04
#define TLV_TYPE_FLAGS  0x13

#define PAIR_SETUP_M1 1
#define PAIR_SETUP_M2 2
#define PAIR_SETUP_M3 3
#define PAIR_SETUP_M4 4
#define PAIR_SETUP_M5 5
#define PAIR_SETUP_M6 6

esp_err_t hap_pair_setup_m1(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  size_t state_len = 0;
  size_t flags_len = 0;
  const uint8_t *state =
      tlv8_find(input, input_len, TLV_TYPE_STATE, &state_len);
  const uint8_t *flags =
      tlv8_find(input, input_len, TLV_TYPE_FLAGS, &flags_len);

  if (!state || state_len != 1 || state[0] != PAIR_SETUP_M1) {
    ESP_LOGE(TAG, "Invalid pair-setup M1 state");
    return ESP_ERR_INVALID_ARG;
  }

  bool transient = false;
  uint32_t flag_bits = 0;
  if (flags && flags_len >= 1 && flags_len <= 4) {
    for (size_t i = 0; i < flags_len; i++) flag_bits |= (uint32_t)flags[i] << (8U * i);
    transient = (flag_bits & 0x10U) != 0;
  }
  size_t method_len = 0;
  const uint8_t *method = tlv8_find(input, input_len, TLV_TYPE_METHOD, &method_len);
  ESP_LOGI(TAG, "pair-setup M1: method=%d flags=0x%x -> %s pairing",
           (method && method_len == 1) ? method[0] : -1, (unsigned)flag_bits,
           transient ? "transient" : "full (HomeKit)");
  session->pair_setup_transient = transient;

  if (session->srp) {
    srp_session_free(session->srp);
  }
  session->srp = srp_session_create();
  if (!session->srp) {
    ESP_LOGE(TAG, "Failed to create SRP session");
    return ESP_ERR_NO_MEM;
  }

#ifdef CONFIG_AIRPLAY_HOMEKIT_SETUP_CODE
  const char *password = transient ? "3939" : CONFIG_AIRPLAY_HOMEKIT_SETUP_CODE;
#else
  const char *password = transient ? "3939" : "0000";
#endif
  esp_err_t err = srp_start(session->srp, "Pair-Setup", password);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start SRP: %d", err);
    return err;
  }

  size_t pk_len = 0;
  const uint8_t *salt = srp_get_salt(session->srp);
  const uint8_t *pk = srp_get_public_key(session->srp, &pk_len);

  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M2);
  tlv8_encode(&enc, TLV_TYPE_SALT, salt, SRP_SALT_BYTES);
  tlv8_encode(&enc, TLV_TYPE_PUBLIC_KEY, pk, pk_len);

  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = PAIR_SETUP_M2;
  return ESP_OK;
}

esp_err_t hap_pair_setup_m3(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  if (!session->srp) {
    ESP_LOGE(TAG, "No SRP session for M3");
    return ESP_ERR_INVALID_STATE;
  }

  size_t state_len = 0;
  size_t pk_len = 0;
  size_t proof_len = 0;
  const uint8_t *state =
      tlv8_find(input, input_len, TLV_TYPE_STATE, &state_len);

  if (!state || state_len != 1 || state[0] != PAIR_SETUP_M3) {
    ESP_LOGE(TAG, "Invalid pair-setup M3 state");
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t client_pk[512];
  if (!tlv8_decode_concat(input, input_len, TLV_TYPE_PUBLIC_KEY, client_pk,
                          sizeof(client_pk), &pk_len)) {
    ESP_LOGE(TAG, "Missing client public key in M3");
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t client_proof[64];
  if (!tlv8_decode_concat(input, input_len, TLV_TYPE_PROOF, client_proof,
                          sizeof(client_proof), &proof_len)) {
    ESP_LOGE(TAG, "Missing client proof in M3");
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = srp_verify_client(session->srp, client_pk, pk_len,
                                    client_proof, proof_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "pair-setup M3: wrong code (%s pairing) - client proof rejected",
             session->pair_setup_transient ? "transient" : "full");
    tlv8_encoder_t enc;
    tlv8_encoder_init(&enc, output, output_capacity);
    tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M4);
    tlv8_encode_byte(&enc, TLV_TYPE_ERROR, 0x02);
    *output_len = tlv8_encoder_size(&enc);
    return ESP_OK;
  }

  if (session->pair_setup_transient) {
    size_t srp_key_len = 0;
    const uint8_t *srp_key = srp_get_session_key(session->srp, &srp_key_len);
    if (!srp_key || srp_key_len == 0) {
      ESP_LOGE(TAG, "Missing SRP session key for transient pairing");
      return ESP_ERR_INVALID_STATE;
    }

    memcpy(session->shared_secret, srp_key, 32);
    hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, srp_key, srp_key_len,
                    (uint8_t *)"Control-Read-Encryption-Key", 27,
                    session->encrypt_key, 32);
    hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, srp_key, srp_key_len,
                    (uint8_t *)"Control-Write-Encryption-Key", 28,
                    session->decrypt_key, 32);
    session->encrypt_nonce = 0;
    session->decrypt_nonce = 0;
    session->session_established = true;
  }

  const uint8_t *server_proof = srp_get_proof(session->srp);

  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M4);
  tlv8_encode(&enc, TLV_TYPE_PROOF, server_proof, SRP_PROOF_BYTES);

  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = PAIR_SETUP_M4;

  return ESP_OK;
}

/* M6 carrying only an error: the controller shows "could not add". */
static esp_err_t m6_error(uint8_t *output, size_t cap, size_t *output_len, uint8_t code) {
  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, cap);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M6);
  tlv8_encode_byte(&enc, TLV_TYPE_ERROR, code);
  *output_len = tlv8_encoder_size(&enc);
  return ESP_OK;
}

esp_err_t hap_pair_setup_m5(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
#ifndef CONFIG_AIRPLAY_HOMEKIT
  (void)session;
  (void)input;
  (void)input_len;
  ESP_LOGW(TAG, "pair-setup M5: HomeKit pairing disabled in menuconfig");
  return m6_error(output, output_capacity, output_len, TLV_ERROR_UNAVAILABLE);
#endif
  if (!session->srp) {
    ESP_LOGE(TAG, "No SRP session for M5");
    return ESP_ERR_INVALID_STATE;
  }

  size_t state_len = 0;
  const uint8_t *state =
      tlv8_find(input, input_len, TLV_TYPE_STATE, &state_len);

  if (!state || state_len != 1 || state[0] != PAIR_SETUP_M5) {
    ESP_LOGE(TAG, "Invalid pair-setup M5 state");
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t encrypted[512];
  size_t encrypted_len = 0;
  if (!tlv8_decode_concat(input, input_len, TLV_TYPE_ENCRYPTED_DATA, encrypted,
                          sizeof(encrypted), &encrypted_len)) {
    ESP_LOGE(TAG, "Missing encrypted data in M5");
    return ESP_ERR_INVALID_ARG;
  }

  size_t srp_key_len = 0;
  const uint8_t *srp_key = srp_get_session_key(session->srp, &srp_key_len);
  if (!srp_key || srp_key_len == 0) {
    ESP_LOGE(TAG, "Missing SRP session key");
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t setup_key[32];
  hap_hkdf_sha512((uint8_t *)"Pair-Setup-Encrypt-Salt", 23, srp_key,
                  srp_key_len, (uint8_t *)"Pair-Setup-Encrypt-Info", 23,
                  setup_key, 32);

  uint8_t nonce[12] = {0, 0, 0, 0, 'P', 'S', '-', 'M', 's', 'g', '0', '5'};
  uint8_t decrypted[512];
  unsigned long long decrypted_len = 0;

  if (crypto_aead_chacha20poly1305_ietf_decrypt(decrypted, &decrypted_len, NULL,
                                                encrypted, encrypted_len, NULL,
                                                0, nonce, setup_key) != 0) {
    ESP_LOGE(TAG, "M5 decryption failed");
    return m6_error(output, output_capacity, output_len, TLV_ERROR_AUTHENTICATION);
  }

  /* Controller: pairing ID, long-term public key, signature over
   * iOSDeviceX || pairing ID || LTPK. */
  size_t id_len = 0, ltpk_len = 0, sig_len = 0;
  const uint8_t *id = tlv8_find(decrypted, (size_t)decrypted_len, TLV_TYPE_IDENTIFIER, &id_len);
  const uint8_t *ltpk =
      tlv8_find(decrypted, (size_t)decrypted_len, TLV_TYPE_PUBLIC_KEY, &ltpk_len);
  const uint8_t *sig =
      tlv8_find(decrypted, (size_t)decrypted_len, TLV_TYPE_SIGNATURE, &sig_len);
  if (!id || id_len == 0 || id_len > HAP_PAIRING_ID_MAX || !ltpk || ltpk_len != 32 ||
      !sig || sig_len != crypto_sign_BYTES) {
    ESP_LOGE(TAG, "M5: missing controller id/key/signature (id=%u key=%u sig=%u)",
             (unsigned)id_len, (unsigned)ltpk_len, (unsigned)sig_len);
    return m6_error(output, output_capacity, output_len, TLV_ERROR_AUTHENTICATION);
  }
  uint8_t info[32 + HAP_PAIRING_ID_MAX + 32];
  hap_hkdf_sha512((const uint8_t *)"Pair-Setup-Controller-Sign-Salt", 31, srp_key,
                  srp_key_len, (const uint8_t *)"Pair-Setup-Controller-Sign-Info", 31,
                  info, 32);
  memcpy(info + 32, id, id_len);
  memcpy(info + 32 + id_len, ltpk, 32);
  if (crypto_sign_verify_detached(sig, info, 32 + id_len + 32, ltpk) != 0) {
    ESP_LOGE(TAG, "M5: controller %.*s signature invalid", (int)id_len, (const char *)id);
    return m6_error(output, output_capacity, output_len, TLV_ERROR_AUTHENTICATION);
  }
  esp_err_t err = hap_pairings_add(id, id_len, ltpk, HAP_PERM_ADMIN);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "M5: storing controller %.*s failed: %s", (int)id_len,
             (const char *)id, esp_err_to_name(err));
    return m6_error(output, output_capacity, output_len,
                    err == ESP_ERR_NO_MEM ? TLV_ERROR_MAX_PEERS : TLV_ERROR_UNKNOWN);
  }
  memcpy(session->controller_id, id, id_len);
  session->controller_id_len = (uint8_t)id_len;
  session->controller_perm = HAP_PERM_ADMIN;
  session->controller_verified = true;

  /* Accessory: AccessoryX || pairing ID || LTPK, signed with the LTSK. The
   * pairing ID is the advertised "pi". */
  char device_id[AIRPLAY_PAIRING_ID_LEN];
  airplay_get_pairing_id(device_id, sizeof(device_id));
  const size_t device_id_len = strlen(device_id);
  hap_hkdf_sha512((const uint8_t *)"Pair-Setup-Accessory-Sign-Salt", 30, srp_key,
                  srp_key_len, (const uint8_t *)"Pair-Setup-Accessory-Sign-Info", 30,
                  info, 32);
  memcpy(info + 32, device_id, device_id_len);
  memcpy(info + 32 + device_id_len, session->device_public_key, 32);
  uint8_t acc_sig[crypto_sign_BYTES];
  crypto_sign_detached(acc_sig, NULL, info, 32 + device_id_len + 32,
                       session->device_secret_key);

  uint8_t sub[160];
  tlv8_encoder_t sub_enc;
  tlv8_encoder_init(&sub_enc, sub, sizeof(sub));
  tlv8_encode(&sub_enc, TLV_TYPE_IDENTIFIER, (const uint8_t *)device_id, device_id_len);
  tlv8_encode(&sub_enc, TLV_TYPE_PUBLIC_KEY, session->device_public_key, 32);
  tlv8_encode(&sub_enc, TLV_TYPE_SIGNATURE, acc_sig, crypto_sign_BYTES);

  uint8_t nonce6[12] = {0, 0, 0, 0, 'P', 'S', '-', 'M', 's', 'g', '0', '6'};
  uint8_t enc6[160 + crypto_aead_chacha20poly1305_ietf_ABYTES];
  unsigned long long enc6_len = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(enc6, &enc6_len, sub, tlv8_encoder_size(&sub_enc),
                                            NULL, 0, NULL, nonce6, setup_key);
  sodium_memzero(setup_key, sizeof(setup_key));

  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_SETUP_M6);
  tlv8_encode(&enc, TLV_TYPE_ENCRYPTED_DATA, enc6, (size_t)enc6_len);
  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = PAIR_SETUP_M6;
  session->encrypt_nonce = 0;
  session->decrypt_nonce = 0;
  ESP_LOGI(TAG, "pair-setup M6: paired with HomeKit controller %.*s (admin)",
           (int)id_len, (const char *)id);
  hap_pairings_log("after pair-setup", true);
  return ESP_OK;
}
