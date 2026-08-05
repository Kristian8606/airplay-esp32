#include <stdlib.h>
#include <string.h>

#include "audio_stream.h"

#include "audio_buffer.h"
#include "audio_decoder.h"
#include "audio_receiver_internal.h"

extern const audio_stream_ops_t audio_stream_realtime_ops;
extern const audio_stream_ops_t audio_stream_buffered_ops;

static bool apply_aac_transient_mute(audio_receiver_state_t *state,
                                     int16_t *buffer, size_t samples,
                                     int channels) {
  if (!audio_decoder_is_aac(state->decoder)) {
    return false;
  }

  if ((state->blocks_read_in_sequence <= 2) &&
      (state->blocks_read_in_sequence != state->blocks_read)) {
    memset(buffer, 0, samples * channels * sizeof(int16_t));
    return true;
  }

  return false;
}

static uint32_t audio_stream_gate_epoch(const audio_receiver_state_t *state) {
  return __atomic_load_n(&state->rtp_gate_epoch, __ATOMIC_ACQUIRE);
}

bool audio_stream_accept_timestamp(audio_receiver_state_t *state,
                                   uint32_t timestamp) {
  if (!state || state->discard_all_until_anchor) {
    return false;
  }

  uint32_t epoch = audio_stream_gate_epoch(state);
  if (epoch == 0) {
    return true;
  }
  if ((int32_t)(timestamp - state->discard_before_rtp) < 0 ||
      (int32_t)(timestamp - state->discard_above_rtp) > 0) {
    return false;
  }

  /* Disarm only the exact window observed by this ordered pre-decode pass.
   * A newer seek increments the epoch, so this compare-exchange cannot clear
   * the replacement window. */
  __atomic_compare_exchange_n(&state->rtp_gate_epoch, &epoch, 0, false,
                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  return true;
}

static bool timestamp_is_gated(const audio_receiver_state_t *state,
                               uint32_t timestamp) {
  if (state->discard_all_until_anchor) {
    return true;
  }
  uint32_t epoch = audio_stream_gate_epoch(state);
  return epoch != 0 &&
         ((int32_t)(timestamp - state->discard_before_rtp) < 0 ||
          (int32_t)(timestamp - state->discard_above_rtp) > 0);
}

bool audio_stream_process_accepted_frame(audio_receiver_state_t *state,
                                         uint32_t timestamp,
                                         const uint8_t *audio_data,
                                         size_t audio_len) {
  if (!state || !state->decoder) {
    return false;
  }

  uint32_t generation = audio_timing_generation_get(&state->timing);
  size_t capacity_samples = 0;
  int16_t *decode_buffer =
      audio_buffer_get_decode_buffer(&state->buffer, &capacity_samples);
  if (!decode_buffer || capacity_samples == 0) {
    return false;
  }

  audio_decode_info_t info = {0};
  int decoded_samples =
      audio_decoder_decode(state->decoder, audio_data, audio_len, decode_buffer,
                           capacity_samples, &info);
  if (decoded_samples <= 0) {
    return false;
  }

  int channels =
      info.channels > 0 ? info.channels : state->stream->format.channels;
  if (channels <= 0) {
    channels = 2;
  }
  apply_aac_transient_mute(state, decode_buffer, (size_t)decoded_samples,
                           channels);

  /* Producer-side checks avoid publishing work decoded across a seek/flush.
   * The consumer-side header generation check is the final guarantee that a
   * frame racing with a buffer clear is never played. */
  if (generation != audio_timing_generation_get(&state->timing) ||
      timestamp_is_gated(state, timestamp)) {
    return false;
  }

  return audio_buffer_queue_decoded(
      &state->buffer, &state->stats, timestamp, generation, decode_buffer,
      (size_t)decoded_samples, channels);
}

bool audio_stream_process_frame(audio_receiver_state_t *state,
                                uint32_t timestamp, const uint8_t *audio_data,
                                size_t audio_len) {
  if (!audio_stream_accept_timestamp(state, timestamp)) {
    return false;
  }
  return audio_stream_process_accepted_frame(state, timestamp, audio_data,
                                             audio_len);
}

audio_stream_t *audio_stream_create_realtime(void) {
  audio_stream_t *stream = calloc(1, sizeof(*stream));
  if (!stream) {
    return NULL;
  }

  stream->ops = &audio_stream_realtime_ops;
  stream->type = AUDIO_STREAM_REALTIME;
  return stream;
}

audio_stream_t *audio_stream_create_buffered(void) {
  audio_stream_t *stream = calloc(1, sizeof(*stream));
  if (!stream) {
    return NULL;
  }

  stream->ops = &audio_stream_buffered_ops;
  stream->type = AUDIO_STREAM_BUFFERED;
  return stream;
}

void audio_stream_destroy(audio_stream_t *stream) {
  if (!stream) {
    return;
  }

  if (stream->ops && stream->ops->destroy) {
    stream->ops->destroy(stream);
    return;
  }

  free(stream);
}

bool audio_stream_uses_buffer(audio_stream_type_t type) {
  return type == AUDIO_STREAM_BUFFERED;
}
