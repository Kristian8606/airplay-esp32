#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef void *httpd_handle_t;
typedef struct { size_t content_len; const char *body; const char *content_type; const char *site; int status; char response[256]; } httpd_req_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
#define HTTP_POST 1
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_TYPE_JSON "application/json"
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t*);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*,const char*,char*,size_t);
esp_err_t httpd_resp_set_status(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_type(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char*,const char*);
esp_err_t httpd_resp_sendstr(httpd_req_t*,const char*);
esp_err_t httpd_resp_send_err(httpd_req_t*,int,const char*);
