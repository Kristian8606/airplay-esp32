#pragma once
#include <stdint.h>
#include <stdbool.h>
int socket_utils_bind_tcp_listener(uint16_t port, int backlog, bool nonblock, uint16_t *bound);
