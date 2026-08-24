#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_receiver.h"
#include "esp_err.h"

typedef bool (*realtime_pcm_sink_t)(uint32_t rtp, int16_t *pcm,
                                    size_t frames, int channels, void *ctx);

typedef struct {
  audio_format_t format;
  audio_encrypt_t encrypt;
  realtime_pcm_sink_t pcm_sink;
  void *pcm_sink_ctx;
} realtime_receiver_config_t;

esp_err_t realtime_receiver_start(uint16_t data_port, uint16_t control_port,
                                  const realtime_receiver_config_t *config);
void realtime_receiver_stop(void);
bool realtime_receiver_is_running(void);
void realtime_receiver_set_client_control(uint32_t client_ip, uint16_t client_control_port);
