#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * RAOP Apple-Challenge signing with the well-known AirPlay private key
 * (RSA PKCS1 v1.5 private encrypt). Uses mbedtls for RSA (libsodium does not
 * support RSA).
 */

/**
 * Build the Apple-Challenge response.
 *
 * Decodes the base64 challenge, appends our IP + MAC, pads to 32 bytes,
 * signs with RSA PKCS1 v1.5, and returns the base64-encoded response
 * (no trailing '=' padding).
 *
 * @param challenge_b64  Base64-encoded Apple-Challenge header value
 * @param ip_addr        Our IPv4 address in network byte order
 * @param mac            Our 6-byte MAC address
 * @param out_b64        Output buffer for base64-encoded response
 * @param out_b64_size   Size of output buffer (at least 343 bytes)
 * @return 0 on success, -1 on failure
 */
int rsa_apple_challenge_response(const char *challenge_b64, uint32_t ip_addr,
                                 const uint8_t mac[6], char *out_b64,
                                 size_t out_b64_size);
