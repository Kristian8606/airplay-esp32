#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rtsp_conn.h"

/**
 * RTSP Message Parsing and Response Building
 */

/**
 * Parsed RTSP request structure
 */
typedef struct {
  char method[32];
  char path[256];
  char protocol[16];
  int cseq;
  char content_type[64];
  size_t content_length;
  const uint8_t *body;
  size_t body_len;
} rtsp_request_t;

/**
 * Parse RTSP request from buffer
 * @param data Raw request data
 * @param len Length of data
 * @param req Output: parsed request (body pointer references original data)
 * @return 0 on success, -1 on parse error
 */
int rtsp_request_parse(const uint8_t *data, size_t len, rtsp_request_t *req);

/**
 * Find end of HTTP/RTSP headers (\r\n\r\n)
 * @param data Request data
 * @param len Length of data
 * @return Pointer to first \r\n of header end, or NULL if not found
 */
const uint8_t *rtsp_find_header_end(const uint8_t *data, size_t len);

/**
 * Parse Content-Length header from request
 * @param request Request string
 * @return Content length, 0 if absent, -1 if malformed or duplicated
 */
int rtsp_parse_content_length(const char *request);

/**
 * Send RTSP response (handles encryption automatically)
 * @param socket Client socket
 * @param conn Connection state
 * @param status_code HTTP status code (200, 404, etc.)
 * @param status_text Status text ("OK", "Not Found", etc.)
 * @param cseq CSeq value from request
 * @param extra_headers Additional headers (NULL if none, must end with \r\n)
 * @param body Response body (NULL if none)
 * @param body_len Body length
 * @return 0 on success, -1 on error
 */
int rtsp_send_response(int socket, rtsp_conn_t *conn, int status_code,
                       const char *status_text, int cseq,
                       const char *extra_headers, const char *body,
                       size_t body_len);

/**
 * Send simple 200 OK response
 */
int rtsp_send_ok(int socket, rtsp_conn_t *conn, int cseq);

/**
 * Send HTTP response (plain HTTP GET /info)
 * @param socket Client socket
 * @param conn Connection state
 * @param status_code HTTP status code
 * @param status_text Status text
 * @param content_type Content-Type header value
 * @param body Response body
 * @param body_len Body length
 * @return 0 on success, -1 on error
 */
int rtsp_send_http_response(int socket, rtsp_conn_t *conn, int status_code,
                            const char *status_text, const char *content_type,
                            const char *body, size_t body_len);
