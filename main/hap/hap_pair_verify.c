#include <string.h>

#include "airplay_identity.h"
#include "hap.h"
#include "hap_internal.h"
#include "hap_pairings.h"
#include "tlv8.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "aes/esp_aes.h"
#include "sodium.h"

static const char *TAG = "hap_verify";

esp_err_t hap_pair_verify_m1(hap_session_t *session, const uint8_t *input,
                             size_t input_len, uint8_t *output,
                             size_t output_capacity, size_t *output_len) {
  size_t state_len = 0;
  const uint8_t *state =
      tlv8_find(input, input_len, TLV_TYPE_STATE, &state_len);
  if (!state || state_len != 1 || state[0] != PAIR_VERIFY_STATE_M1) {
    ESP_LOGE(TAG, "Invalid M1 state");
    return ESP_ERR_INVALID_ARG;
  }

  size_t client_pk_len = 0;
  const uint8_t *client_pk =
      tlv8_find(input, input_len, TLV_TYPE_PUBLIC_KEY, &client_pk_len);
  if (!client_pk || client_pk_len != HAP_X25519_KEY_SIZE) {
    ESP_LOGE(TAG, "Invalid M1 public key");
    return ESP_ERR_INVALID_ARG;
  }

  memcpy(session->client_public_key, client_pk, HAP_X25519_KEY_SIZE);

  if (crypto_scalarmult(session->shared_secret, session->session_secret_key,
                        session->client_public_key) != 0) {
    ESP_LOGE(TAG, "X25519 key exchange failed");
    return ESP_FAIL;
  }

  /* Accessory pairing identifier. A controller that knows this receiver
   * from a home (added over transient pairing + /pair-add, so it never got
   * an M6) only has the TXT/info "pi" and "pk" to go by; the device ID (MAC)
   * was used so far. Until it is known which one the controller expects,
   * present "pi" first and the device ID when the controller starts over
   * with a new M1 on the same connection (it rejected the previous M2). */
  char device_id[AIRPLAY_PAIRING_ID_LEN];
  session->pv_id_is_pi = (session->pv_attempts % 2U) == 0U;
  if (session->pv_id_is_pi) {
    airplay_get_pairing_id(device_id, sizeof(device_id));
  } else {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  const size_t device_id_len = strlen(device_id);
  session->pv_attempts++;
  ESP_LOGI(TAG, "pair-verify M1 #%u: presenting accessory id %s (%s)%s",
           (unsigned)session->pv_attempts, device_id, session->pv_id_is_pi ? "pi" : "device ID",
           session->pv_attempts > 1 ? " - controller did not accept the previous M2" : "");

  uint8_t accessory_info[128];
  size_t accessory_info_len = 0;
  memcpy(accessory_info + accessory_info_len, session->session_public_key,
         HAP_X25519_KEY_SIZE);
  accessory_info_len += HAP_X25519_KEY_SIZE;
  memcpy(accessory_info + accessory_info_len, device_id, device_id_len);
  accessory_info_len += device_id_len;
  memcpy(accessory_info + accessory_info_len, session->client_public_key,
         HAP_X25519_KEY_SIZE);
  accessory_info_len += HAP_X25519_KEY_SIZE;

  uint8_t signature[crypto_sign_BYTES];
  crypto_sign_detached(signature, NULL, accessory_info, accessory_info_len,
                       session->device_secret_key);

  uint8_t sub_tlv[256];
  tlv8_encoder_t sub_enc;
  tlv8_encoder_init(&sub_enc, sub_tlv, sizeof(sub_tlv));
  tlv8_encode(&sub_enc, TLV_TYPE_IDENTIFIER, (const uint8_t *)device_id,
              device_id_len);
  tlv8_encode(&sub_enc, TLV_TYPE_SIGNATURE, signature, crypto_sign_BYTES);

  uint8_t session_key[32];
  hap_hkdf_sha512((uint8_t *)"Pair-Verify-Encrypt-Salt", 24,
                  session->shared_secret, HAP_X25519_KEY_SIZE,
                  (uint8_t *)"Pair-Verify-Encrypt-Info", 24, session_key, 32);

  uint8_t nonce[12] = {0, 0, 0, 0, 'P', 'V', '-', 'M', 's', 'g', '0', '2'};
  uint8_t encrypted[256 + crypto_aead_chacha20poly1305_ietf_ABYTES];
  unsigned long long encrypted_len = 0;

  crypto_aead_chacha20poly1305_ietf_encrypt(encrypted, &encrypted_len, sub_tlv,
                                            tlv8_encoder_size(&sub_enc), NULL,
                                            0, NULL, nonce, session_key);

  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_VERIFY_STATE_M2);
  tlv8_encode(&enc, TLV_TYPE_PUBLIC_KEY, session->session_public_key,
              HAP_X25519_KEY_SIZE);
  tlv8_encode(&enc, TLV_TYPE_ENCRYPTED_DATA, encrypted, (size_t)encrypted_len);

  *output_len = tlv8_encoder_size(&enc);
  session->pair_verify_state = PAIR_VERIFY_STATE_M2;

  memcpy(session->encrypt_key, session_key, sizeof(session_key));

  return ESP_OK;
}

esp_err_t hap_pair_verify_m3(hap_session_t *session, const uint8_t *input,
                             size_t input_len, uint8_t *output,
                             size_t output_capacity, size_t *output_len) {
  size_t state_len = 0;
  const uint8_t *state =
      tlv8_find(input, input_len, TLV_TYPE_STATE, &state_len);
  if (!state || state_len != 1 || state[0] != PAIR_VERIFY_STATE_M3) {
    ESP_LOGE(TAG, "Invalid M3 state");
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t encrypted[512];
  size_t encrypted_len = 0;
  if (!tlv8_decode_concat(input, input_len, TLV_TYPE_ENCRYPTED_DATA, encrypted,
                          sizeof(encrypted), &encrypted_len)) {
    ESP_LOGE(TAG, "Missing M3 encrypted data");
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t nonce[12] = {0, 0, 0, 0, 'P', 'V', '-', 'M', 's', 'g', '0', '3'};
  uint8_t decrypted[512];
  unsigned long long decrypted_len = 0;

  if (crypto_aead_chacha20poly1305_ietf_decrypt(
          decrypted, &decrypted_len, NULL, encrypted, encrypted_len, NULL, 0,
          nonce, session->encrypt_key) != 0) {
    ESP_LOGE(TAG, "M3 decryption failed");
    tlv8_encoder_t enc;
    tlv8_encoder_init(&enc, output, output_capacity);
    tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_VERIFY_STATE_M4);
    tlv8_encode_byte(&enc, TLV_TYPE_ERROR, TLV_ERROR_AUTHENTICATION);
    *output_len = tlv8_encoder_size(&enc);
    return ESP_ERR_INVALID_STATE;
  }

  /* A controller this receiver is paired with (HomeKit) must prove it owns
   * the stored long-term key: signature over iOSDevicePublicKey || pairing
   * ID || AccessoryPublicKey. Other controllers keep the open AirPlay access
   * (acl=0) they had before. */
  size_t id_len = 0, sig_len = 0;
  const uint8_t *id = tlv8_find(decrypted, (size_t)decrypted_len, TLV_TYPE_IDENTIFIER, &id_len);
  const uint8_t *sig =
      tlv8_find(decrypted, (size_t)decrypted_len, TLV_TYPE_SIGNATURE, &sig_len);
  uint8_t ltpk[32];
  uint8_t perm = 0;
  session->controller_verified = false;
  session->controller_id_len = 0;
  if (id && id_len > 0 && id_len <= HAP_PAIRING_ID_MAX &&
      hap_pairings_find(id, id_len, ltpk, &perm)) {
    uint8_t info[32 + HAP_PAIRING_ID_MAX + 32];
    memcpy(info, session->client_public_key, 32);
    memcpy(info + 32, id, id_len);
    memcpy(info + 32 + id_len, session->session_public_key, 32);
    if (!sig || sig_len != crypto_sign_BYTES ||
        crypto_sign_verify_detached(sig, info, 32 + id_len + 32, ltpk) != 0) {
      ESP_LOGE(TAG, "pair-verify: paired controller %.*s failed its signature check",
               (int)id_len, (const char *)id);
      tlv8_encoder_t enc;
      tlv8_encoder_init(&enc, output, output_capacity);
      tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_VERIFY_STATE_M4);
      tlv8_encode_byte(&enc, TLV_TYPE_ERROR, TLV_ERROR_AUTHENTICATION);
      *output_len = tlv8_encoder_size(&enc);
      return ESP_ERR_INVALID_STATE;
    }
    memcpy(session->controller_id, id, id_len);
    session->controller_id_len = (uint8_t)id_len;
    session->controller_perm = perm;
    session->controller_verified = true;
    ESP_LOGI(TAG, "pair-verify: HomeKit controller %.*s (%s), accepted our %s",
             (int)id_len, (const char *)id, (perm & HAP_PERM_ADMIN) ? "admin" : "user",
             session->pv_id_is_pi ? "pi" : "device ID");
  } else if (id && id_len > 0) {
    ESP_LOGI(TAG, "pair-verify: controller %.*s is not paired (open access)",
             (int)(id_len > HAP_PAIRING_ID_MAX ? HAP_PAIRING_ID_MAX : id_len),
             (const char *)id);
  }

  hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, session->shared_secret,
                  HAP_X25519_KEY_SIZE, (uint8_t *)"Control-Read-Encryption-Key",
                  27, session->encrypt_key, 32);

  hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, session->shared_secret,
                  HAP_X25519_KEY_SIZE,
                  (uint8_t *)"Control-Write-Encryption-Key", 28,
                  session->decrypt_key, 32);

  tlv8_encoder_t enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV_TYPE_STATE, PAIR_VERIFY_STATE_M4);

  *output_len = tlv8_encoder_size(&enc);
  session->pair_verify_state = PAIR_VERIFY_STATE_M4;
  session->session_established = true;

  return ESP_OK;
}

esp_err_t hap_pair_verify_m1_raw(hap_session_t *session, const uint8_t *input,
                                 size_t input_len, uint8_t *output,
                                 size_t output_capacity, size_t *output_len) {
  const uint8_t *client_epk;

  if (input_len >= 68) {
    client_epk = input + 4;
  } else if (input_len >= 64) {
    client_epk = input;
  } else if (input_len >= 32) {
    client_epk = input;
  } else {
    ESP_LOGE(TAG, "Input too short for pair-verify: %zu", input_len);
    return ESP_ERR_INVALID_ARG;
  }

  memcpy(session->client_public_key, client_epk, HAP_X25519_KEY_SIZE);

  if (crypto_scalarmult(session->shared_secret, session->session_secret_key,
                        session->client_public_key) != 0) {
    ESP_LOGE(TAG, "X25519 key exchange failed");
    return ESP_FAIL;
  }

  uint8_t aes_key[16];
  uint8_t aes_iv[16];

  {
    crypto_hash_sha512_state state;
    uint8_t hash[64];
    crypto_hash_sha512_init(&state);
    crypto_hash_sha512_update(&state, (const uint8_t *)"Pair-Verify-AES-Key",
                              19);
    crypto_hash_sha512_update(&state, session->shared_secret, 32);
    crypto_hash_sha512_final(&state, hash);
    memcpy(aes_key, hash, sizeof(aes_key));
  }

  {
    crypto_hash_sha512_state state;
    uint8_t hash[64];
    crypto_hash_sha512_init(&state);
    crypto_hash_sha512_update(&state, (const uint8_t *)"Pair-Verify-AES-IV",
                              18);
    crypto_hash_sha512_update(&state, session->shared_secret, 32);
    crypto_hash_sha512_final(&state, hash);
    memcpy(aes_iv, hash, sizeof(aes_iv));
  }

  uint8_t signed_data[64];
  memcpy(signed_data, session->session_public_key, 32);
  memcpy(signed_data + 32, session->client_public_key, 32);

  uint8_t signature[64];
  crypto_sign_detached(signature, NULL, signed_data, sizeof(signed_data),
                       session->device_secret_key);

  if (output_capacity < 96) {
    ESP_LOGE(TAG, "Output buffer too small for M2 (need 96, have %zu)",
             output_capacity);
    return ESP_ERR_NO_MEM;
  }

  memcpy(output, session->session_public_key, 32);

  esp_aes_context aes_ctx;
  esp_aes_init(&aes_ctx);
  int aes_ret = esp_aes_setkey(&aes_ctx, aes_key, 128);
  if (aes_ret != 0) {
    esp_aes_free(&aes_ctx);
    ESP_LOGE(TAG, "Pair-Verify AES key setup failed: %d", aes_ret);
    return ESP_FAIL;
  }

  uint8_t stream_block[16] = {0};
  size_t nc_off = 0;
  uint8_t nonce_counter[16];
  memcpy(nonce_counter, aes_iv, sizeof(nonce_counter));

  aes_ret = esp_aes_crypt_ctr(&aes_ctx, sizeof(signature), &nc_off, nonce_counter,
                        stream_block, signature, output + 32);
  esp_aes_free(&aes_ctx);
  if (aes_ret != 0) {
    ESP_LOGE(TAG, "Pair-Verify AES encryption failed: %d", aes_ret);
    return ESP_FAIL;
  }

  *output_len = 96;
  session->pair_verify_state = PAIR_VERIFY_STATE_M2;

  memcpy(session->encrypt_key, aes_key, sizeof(aes_key));
  memcpy(session->decrypt_key, aes_iv, sizeof(aes_iv));

  return ESP_OK;
}

esp_err_t hap_pair_verify_m3_raw(hap_session_t *session, const uint8_t *input,
                                 size_t input_len, uint8_t *output,
                                 size_t output_capacity, size_t *output_len) {
  (void)output_capacity;

  const uint8_t *encrypted_sig;

  if (input_len >= 68) {
    encrypted_sig = input + 4;
  } else if (input_len >= 64) {
    encrypted_sig = input;
  } else {
    ESP_LOGE(TAG, "Input too short for M3: %zu", input_len);
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t aes_key[16];
  uint8_t aes_iv[16];

  {
    crypto_hash_sha512_state state;
    uint8_t hash[64];
    crypto_hash_sha512_init(&state);
    crypto_hash_sha512_update(&state, (const uint8_t *)"Pair-Verify-AES-Key",
                              19);
    crypto_hash_sha512_update(&state, session->shared_secret, 32);
    crypto_hash_sha512_final(&state, hash);
    memcpy(aes_key, hash, sizeof(aes_key));
  }

  {
    crypto_hash_sha512_state state;
    uint8_t hash[64];
    crypto_hash_sha512_init(&state);
    crypto_hash_sha512_update(&state, (const uint8_t *)"Pair-Verify-AES-IV",
                              18);
    crypto_hash_sha512_update(&state, session->shared_secret, 32);
    crypto_hash_sha512_final(&state, hash);
    memcpy(aes_iv, hash, sizeof(aes_iv));
  }

  uint8_t client_signature[64];
  esp_aes_context aes_ctx;
  esp_aes_init(&aes_ctx);
  int aes_ret = esp_aes_setkey(&aes_ctx, aes_key, 128);
  if (aes_ret != 0) {
    esp_aes_free(&aes_ctx);
    ESP_LOGE(TAG, "Pair-Verify AES key setup failed: %d", aes_ret);
    return ESP_FAIL;
  }

  uint8_t stream_block[16] = {0};
  size_t nc_off = 0;
  uint8_t nonce_counter[16];
  memcpy(nonce_counter, aes_iv, sizeof(nonce_counter));

  aes_ret = esp_aes_crypt_ctr(&aes_ctx, sizeof(client_signature), &nc_off,
                        nonce_counter, stream_block, encrypted_sig,
                        client_signature);
  esp_aes_free(&aes_ctx);
  if (aes_ret != 0) {
    ESP_LOGE(TAG, "Pair-Verify AES decryption failed: %d", aes_ret);
    return ESP_FAIL;
  }

  ESP_LOGW(TAG, "Skipping signature verification (transient pairing)");

  hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, session->shared_secret,
                  HAP_X25519_KEY_SIZE, (uint8_t *)"Control-Read-Encryption-Key",
                  27, session->decrypt_key, 32);

  hap_hkdf_sha512((uint8_t *)"Control-Salt", 12, session->shared_secret,
                  HAP_X25519_KEY_SIZE,
                  (uint8_t *)"Control-Write-Encryption-Key", 28,
                  session->encrypt_key, 32);

  session->encrypt_nonce = 0;
  session->decrypt_nonce = 0;

  *output_len = 0;
  session->pair_verify_state = PAIR_VERIFY_STATE_M4;
  session->session_established = true;

  return ESP_OK;
}
