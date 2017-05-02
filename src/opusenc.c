/* Copyright (C)2002-2017 Jean-Marc Valin
   Copyright (C)2007-2013 Xiph.Org Foundation
   Copyright (C)2008-2013 Gregory Maxwell
   File: opusenc.c

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <opus_multistream.h>
#include "opusenc.h"
#include "opus_header.h"
#include "speex_resampler.h"
#include "picture.h"

#define MAX_CHANNELS 8

#define LPC_PADDING 120

/* Allow up to 2 seconds for delayed decision. */
#define MAX_LOOKAHEAD 96000
/* We can't have a circular buffer (because of delayed decision), so let's not copy too often. */
#define BUFFER_EXTRA 24000

#define BUFFER_SAMPLES (MAX_LOOKAHEAD + BUFFER_EXTRA)

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define MAX_PACKET_SIZE (1276*8)

static int oe_write_page(ogg_page *page, OpusEncCallbacks *cb, void *user_data)
{
   int err;
   err = cb->write(user_data, page->header, page->header_len);
   if (err) return -1;
   err = cb->write(user_data, page->body, page->body_len);
   if (err) return -1;
   return page->header_len+page->body_len;
}

struct StdioObject {
  FILE *file;
};

typedef struct EncStream EncStream;

struct EncStream {
  void *user_data;
  ogg_stream_state os;
  int serialno_is_set;
  int serialno;
  int stream_is_init;
  int packetno;
  char *comment;
  int comment_length;
  int seen_file_icons;
  int close_at_end;
  int header_is_frozen;
  ogg_int64_t end_granule;
  EncStream *next;
};

struct OggOpusEnc {
  OpusMSEncoder *st;
  int rate;
  int channels;
  float *buffer;
  int buffer_start;
  int buffer_end;
  SpeexResamplerState *re;
  int frame_size;
  int decision_delay;
  int max_ogg_delay;
  ogg_int64_t curr_granule;
  ogg_int64_t write_granule;
  ogg_int64_t last_page_granule;
  OpusEncCallbacks callbacks;
  OpusHeader header;
  int comment_padding;
  EncStream *streams;
  EncStream *last_stream;
};

static int oe_flush_page(OggOpusEnc *enc) {
  ogg_page og;
  int ret;
  int written = 0;
  while ( (ret = ogg_stream_flush(&enc->streams->os, &og)) ) {
    if (!ret) break;
    ret = oe_write_page(&og, &enc->callbacks, enc->streams->user_data);
    if (ret == -1) {
      return -1;
    }
    written += ret;
  }
  return written;
}

int stdio_write(void *user_data, const unsigned char *ptr, int len) {
  struct StdioObject *obj = (struct StdioObject*)user_data;
  return fwrite(ptr, 1, len, obj->file) != (size_t)len;
}

int stdio_close(void *user_data) {
  struct StdioObject *obj = (struct StdioObject*)user_data;
  int ret = fclose(obj->file);
  free(obj);
  return ret;
}

static const OpusEncCallbacks stdio_callbacks = {
  stdio_write,
  stdio_close
};

/* Create a new OggOpus file. */
OggOpusEnc *ope_create_file(const char *path, int rate, int channels, int family, int *error) {
  OggOpusEnc *enc;
  struct StdioObject *obj;
  obj = malloc(sizeof(*obj));
  enc = ope_create_callbacks(&stdio_callbacks, obj, rate, channels, family, error);
  if (enc == NULL || (error && *error)) {
    return NULL;
  }
  obj->file = fopen(path, "wb");
  if (!obj->file) {
    if (error) *error = OPE_CANNOT_OPEN;
    /* FIXME: Destroy the encoder properly. */
    free(obj);
    return NULL;
  }
  return enc;
}

EncStream *stream_create() {
  EncStream *stream;
  stream = malloc(sizeof(*stream));
  if (!stream) return NULL;
  stream->next = NULL;
  stream->close_at_end = 1;
  stream->serialno_is_set = 0;
  stream->seen_file_icons = 0;
  stream->stream_is_init = 0;
  stream->header_is_frozen = 0;
  stream->comment = NULL;
  comment_init(&stream->comment, &stream->comment_length, opus_get_version_string());
  if (!stream->comment) goto fail;
  {
    char encoder_string[1024];
    snprintf(encoder_string, sizeof(encoder_string), "%s version %s", PACKAGE_NAME, PACKAGE_VERSION);
    comment_add(&stream->comment, &stream->comment_length, "ENCODER", encoder_string);
  }
  return stream;
fail:
  if (stream->comment) free(stream->comment);
  free(stream);
  return NULL;
}

static void stream_destroy(EncStream *stream) {
  if (stream->comment) free(stream->comment);
  if (stream->stream_is_init) ogg_stream_clear(&stream->os);
  free(stream);
}

/* Create a new OggOpus file (callback-based). */
OggOpusEnc *ope_create_callbacks(const OpusEncCallbacks *callbacks, void *user_data,
    int rate, int channels, int family, int *error) {
  OpusMSEncoder *st=NULL;
  OggOpusEnc *enc=NULL;
  int ret;
  if (family != 0 && family != 1 && family != 255) {
    if (error) *error = OPE_UNIMPLEMENTED;
    return NULL;
  }
  if (channels <= 0 || channels > 255) {
    if (error) *error = OPE_BAD_ARG;
    return NULL;
  }
  /* FIXME: Add resampling support. */
  if (rate <= 0) {
    if (error) *error = OPE_BAD_ARG;
    return NULL;
  }

  if ( (enc = malloc(sizeof(*enc))) == NULL) goto fail;
  enc->streams = NULL;
  if ( (enc->streams = stream_create()) == NULL) goto fail;
  enc->streams->next = NULL;
  enc->last_stream = enc->streams;
  enc->rate = rate;
  enc->channels = channels;
  enc->frame_size = 960;
  enc->decision_delay = 96000;
  enc->max_ogg_delay = 48000;
  enc->comment_padding = 512;
  enc->header.channels=channels;
  enc->header.channel_mapping=family;
  enc->header.input_sample_rate=rate;
  enc->header.gain=0;
  st=opus_multistream_surround_encoder_create(48000, channels, enc->header.channel_mapping,
      &enc->header.nb_streams, &enc->header.nb_coupled,
      enc->header.stream_map, OPUS_APPLICATION_AUDIO, &ret);
  if (! (ret == OPUS_OK && st != NULL) ) {
    goto fail;
  }
  if (rate != 48000) {
    enc->re = speex_resampler_init(channels, rate, 48000, 5, NULL);
    if (enc->re == NULL) goto fail;
    speex_resampler_skip_zeros(enc->re);
  } else {
    enc->re = NULL;
  }
  opus_multistream_encoder_ctl(st, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_20_MS));
  {
    opus_int32 tmp;
    int ret;
    ret = opus_multistream_encoder_ctl(st, OPUS_GET_LOOKAHEAD(&tmp));
    if (ret == OPUS_OK) enc->header.preskip = tmp;
    else enc->header.preskip = 0;
  }
  enc->curr_granule = 0;
  enc->write_granule = 0;
  enc->last_page_granule = 0;
  if ( (enc->buffer = malloc(sizeof(*enc->buffer)*BUFFER_SAMPLES*channels)) == NULL) goto fail;
  enc->buffer_start = enc->buffer_end = 0;
  enc->st = st;
  enc->callbacks = *callbacks;
  enc->streams->user_data = user_data;
  if (error) *error = OPUS_OK;
  return enc;
fail:
  if (enc) {
    free(enc);
    if (enc->buffer) free(enc->buffer);
    if (enc->streams) stream_destroy(enc->streams);
  }
  if (st) {
    opus_multistream_encoder_destroy(st);
  }
  return NULL;
}

static void init_stream(OggOpusEnc *enc) {
  time_t start_time;
  assert(!enc->streams->stream_is_init);
  if (!enc->streams->serialno_is_set) {
    start_time = time(NULL);
    srand(((getpid()&65535)<<15)^start_time);

    enc->streams->serialno = rand();
  }
  
  if (ogg_stream_init(&enc->streams->os, enc->streams->serialno) == -1) {
    assert(0);
    /* FIXME: How the hell do we handle that? */
  }
  comment_pad(&enc->streams->comment, &enc->streams->comment_length, enc->comment_padding);

  /*Write header*/
  {
    ogg_packet op;
    /*The Identification Header is 19 bytes, plus a Channel Mapping Table for
      mapping families other than 0. The Channel Mapping Table is 2 bytes +
      1 byte per channel. Because the maximum number of channels is 255, the
      maximum size of this header is 19 + 2 + 255 = 276 bytes.*/
    unsigned char header_data[276];
    int packet_size = opus_header_to_packet(&enc->header, header_data, sizeof(header_data));
    op.packet=header_data;
    op.bytes=packet_size;
    op.b_o_s=1;
    op.e_o_s=0;
    op.granulepos=0;
    op.packetno=0;
    ogg_stream_packetin(&enc->streams->os, &op);
    oe_flush_page(enc);

    op.packet = (unsigned char *)enc->streams->comment;
    op.bytes = enc->streams->comment_length;
    op.b_o_s = 0;
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = 1;
    ogg_stream_packetin(&enc->streams->os, &op);
    oe_flush_page(enc);
  }
  enc->streams->stream_is_init = 1;
  enc->streams->packetno = 2;
}

static void shift_buffer(OggOpusEnc *enc) {
    memmove(enc->buffer, &enc->buffer[enc->channels*enc->buffer_start], enc->channels*(enc->buffer_end-enc->buffer_start)*sizeof(*enc->buffer));
    enc->buffer_end -= enc->buffer_start;
    enc->buffer_start = 0;
}

static void encode_buffer(OggOpusEnc *enc) {
  /* Round up when converting the granule pos because the decoder will round down. */
  ogg_int64_t end_granule48k = (enc->streams->end_granule*48000 + enc->rate - 1)/enc->rate + enc->header.preskip;
  while (enc->buffer_end-enc->buffer_start > enc->frame_size + enc->decision_delay) {
    int flush_needed;
    ogg_packet op;
    ogg_page og;
    int nbBytes;
    unsigned char packet[MAX_PACKET_SIZE];
    nbBytes = opus_multistream_encode_float(enc->st, &enc->buffer[enc->channels*enc->buffer_start],
        enc->buffer_end-enc->buffer_start, packet, MAX_PACKET_SIZE);
    /* FIXME: How do we handle failure here. */
    assert(nbBytes > 0);
    enc->curr_granule += enc->frame_size;
    op.packet=packet;
    op.bytes=nbBytes;
    op.b_o_s=0;
    op.packetno=enc->streams->packetno++;
    op.granulepos=enc->curr_granule;
    op.e_o_s=enc->curr_granule >= end_granule48k;
    if (op.e_o_s) op.granulepos=end_granule48k;
    ogg_stream_packetin(&enc->streams->os, &op);
    /* FIXME: Also flush on too many segments. */
    flush_needed = op.e_o_s || enc->curr_granule - enc->last_page_granule > enc->max_ogg_delay;
    if (flush_needed) {
      while (ogg_stream_flush_fill(&enc->streams->os, &og, 255*255)) {
        if (ogg_page_packets(&og) != 0) enc->last_page_granule = ogg_page_granulepos(&og);
        int ret = oe_write_page(&og, &enc->callbacks, enc->streams->user_data);
        /* FIXME: what do we do if this fails? */
        assert(ret != -1);
      }
    } else {
      while (ogg_stream_pageout_fill(&enc->streams->os, &og, 255*255)) {
        if (ogg_page_packets(&og) != 0) enc->last_page_granule = ogg_page_granulepos(&og);
        int ret = oe_write_page(&og, &enc->callbacks, enc->streams->user_data);
        /* FIXME: what do we do if this fails? */
        assert(ret != -1);
      }
    }
    if (op.e_o_s) {
      EncStream *tmp;
      tmp = enc->streams->next;
      if (enc->streams->close_at_end) enc->callbacks.close(enc->streams->user_data);
      stream_destroy(enc->streams);
      enc->streams = tmp;
      if (!tmp) enc->last_stream = 0;
      return;
    }
    enc->buffer_start += enc->frame_size;
  }
  /* If we've reached the end of the buffer, move everything back to the front. */
  if (enc->buffer_end == BUFFER_SAMPLES) {
    shift_buffer(enc);
  }
  /* This function must never leave the buffer full. */
  assert(enc->buffer_end < BUFFER_SAMPLES);
}

/* Add/encode any number of float samples to the file. */
int ope_write_float(OggOpusEnc *enc, const float *pcm, int samples_per_channel) {
  int channels = enc->channels;
  enc->last_stream->header_is_frozen = 1;
  if (!enc->streams->stream_is_init) init_stream(enc);
  if (samples_per_channel < 0) return OPE_BAD_ARG;
  enc->write_granule += samples_per_channel;
  enc->last_stream->end_granule = enc->write_granule;
  do {
    int i;
    spx_uint32_t in_samples, out_samples;
    out_samples = BUFFER_SAMPLES-enc->buffer_end;
    if (enc->re != NULL) {
      in_samples = samples_per_channel;
      speex_resampler_process_interleaved_float(enc->re, pcm, &in_samples, &enc->buffer[channels*enc->buffer_end], &out_samples);
    } else {
      int curr;
      curr = MIN((spx_uint32_t)samples_per_channel, out_samples);
      for (i=0;i<channels*curr;i++) {
      enc->buffer[channels*enc->buffer_end+i] = pcm[i];
      }
      in_samples = out_samples = curr;
    }
    enc->buffer_end += out_samples;
    pcm += in_samples*channels;
    samples_per_channel -= in_samples;
    encode_buffer(enc);
  } while (samples_per_channel > 0);
  return OPE_OK;
}

#define CONVERT_BUFFER 256

/* Add/encode any number of int16 samples to the file. */
int ope_write(OggOpusEnc *enc, const opus_int16 *pcm, int samples_per_channel) {
  int channels = enc->channels;
  enc->last_stream->header_is_frozen = 1;
  if (!enc->streams->stream_is_init) init_stream(enc);
  if (samples_per_channel < 0) return OPE_BAD_ARG;
  enc->write_granule += samples_per_channel;
  enc->last_stream->end_granule = enc->write_granule;
  do {
    int i;
    spx_uint32_t in_samples, out_samples;
    out_samples = BUFFER_SAMPLES-enc->buffer_end;
    if (enc->re != NULL) {
      float buf[CONVERT_BUFFER*MAX_CHANNELS];
      in_samples = MIN(CONVERT_BUFFER, samples_per_channel);
      for (i=0;i<channels*(int)in_samples;i++) {
        buf[i] = (1.f/32768)*pcm[i];
      }
      speex_resampler_process_interleaved_float(enc->re, buf, &in_samples, &enc->buffer[channels*enc->buffer_end], &out_samples);
    } else {
      int curr;
      curr = MIN((spx_uint32_t)samples_per_channel, out_samples);
      for (i=0;i<channels*curr;i++) {
        enc->buffer[channels*enc->buffer_end+i] = (1.f/32768)*pcm[i];
      }
      in_samples = out_samples = curr;
    }
    enc->buffer_end += out_samples;
    pcm += in_samples*channels;
    samples_per_channel -= in_samples;
    encode_buffer(enc);
  } while (samples_per_channel > 0);
  return OPE_OK;
}

static void finalize_all_streams(OggOpusEnc *enc) {
  /* FIXME: Use a better value. */
  int pad_samples = 3000;
  if (!enc->streams->stream_is_init) init_stream(enc);
  shift_buffer(enc);
  /* FIXME: Do LPC extension instead. */
  memset(&enc->buffer[enc->channels*enc->buffer_end], 0, pad_samples*enc->channels);
  enc->decision_delay = 0;
  enc->buffer_end += pad_samples;
  assert(enc->buffer_end <= BUFFER_SAMPLES);
  encode_buffer(enc);
  assert(enc->streams == NULL);
}

/* Close/finalize the stream. */
int ope_close_and_free(OggOpusEnc *enc) {
  finalize_all_streams(enc);
  free(enc->buffer);
  opus_multistream_encoder_destroy(enc->st);
  if (enc->re) speex_resampler_destroy(enc->re);
  free(enc);
  return OPE_OK;
}

/* Ends the stream and create a new stream within the same file. */
int ope_chain_current(OggOpusEnc *enc) {
  enc->last_stream->close_at_end = 0;
  return ope_continue_new_callbacks(enc, enc->last_stream->user_data);
}

/* Ends the stream and create a new file. */
int ope_continue_new_file(OggOpusEnc *enc, const char *path) {
  (void)enc;
  (void)path;
  return OPE_UNIMPLEMENTED;
}

/* Ends the stream and create a new file (callback-based). */
int ope_continue_new_callbacks(OggOpusEnc *enc, void *user_data) {
  EncStream *new_stream;
  assert(enc->streams);
  assert(enc->last_stream);
  new_stream = stream_create();
  new_stream->user_data = user_data;
  enc->last_stream->next = new_stream;
  enc->last_stream = new_stream;
  return OPE_UNIMPLEMENTED;
}

/* Add a comment to the file (can only be called before encoding samples). */
int ope_add_comment(OggOpusEnc *enc, const char *tag, const char *val) {
  if (enc->last_stream->header_is_frozen) return OPE_TOO_LATE;
  if (enc->last_stream->stream_is_init) return OPE_TOO_LATE;
  if (comment_add(&enc->last_stream->comment, &enc->last_stream->comment_length, tag, val)) return OPE_INTERNAL_ERROR;
  return OPE_OK;
}

int ope_add_picture(OggOpusEnc *enc, const char *spec) {
  const char *error_message;
  char *picture_data;
  if (enc->last_stream->header_is_frozen) return OPE_TOO_LATE;
  if (enc->last_stream->stream_is_init) return OPE_TOO_LATE;
  picture_data = parse_picture_specification(spec, &error_message, &enc->last_stream->seen_file_icons);
  if(picture_data==NULL){
    /* FIXME: return proper errors rather than printing a message. */
    fprintf(stderr,"Error parsing picture option: %s\n",error_message);
    return OPE_BAD_ARG;
  }
  comment_add(&enc->last_stream->comment, &enc->last_stream->comment_length, "METADATA_BLOCK_PICTURE", picture_data);
  free(picture_data);
  return OPE_OK;
}

/* Sets the Opus comment vendor string (optional, defaults to library info). */
int ope_set_vendor_string(OggOpusEnc *enc, const char *vendor) {
  if (enc->last_stream->header_is_frozen) return OPE_TOO_LATE;
  if (enc->last_stream->stream_is_init) return OPE_TOO_LATE;
  (void)vendor;
  return OPE_UNIMPLEMENTED;
}

int ope_flush_header(OggOpusEnc *enc) {
  if (enc->last_stream->header_is_frozen) return OPE_TOO_LATE;
  if (enc->last_stream->stream_is_init) return OPE_TOO_LATE;
  else init_stream(enc);
  return OPE_OK;
}

/* Goes straight to the libopus ctl() functions. */
int ope_encoder_ctl(OggOpusEnc *enc, int request, ...) {
  int ret;
  va_list ap;
  va_start(ap, request);
  switch (request) {
    case OPUS_SET_APPLICATION_REQUEST:
    case OPUS_SET_BITRATE_REQUEST:
    case OPUS_SET_MAX_BANDWIDTH_REQUEST:
    case OPUS_SET_VBR_REQUEST:
    case OPUS_SET_BANDWIDTH_REQUEST:
    case OPUS_SET_COMPLEXITY_REQUEST:
    case OPUS_SET_INBAND_FEC_REQUEST:
    case OPUS_SET_PACKET_LOSS_PERC_REQUEST:
    case OPUS_SET_DTX_REQUEST:
    case OPUS_SET_VBR_CONSTRAINT_REQUEST:
    case OPUS_SET_FORCE_CHANNELS_REQUEST:
    case OPUS_SET_SIGNAL_REQUEST:
    case OPUS_SET_LSB_DEPTH_REQUEST:
    case OPUS_SET_PREDICTION_DISABLED_REQUEST:
#ifdef OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST
    case OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST:
#endif
    {
      opus_int32 value = va_arg(ap, opus_int32);
      ret = opus_multistream_encoder_ctl(enc->st, request, value);
    }
    break;
    case OPUS_SET_EXPERT_FRAME_DURATION_REQUEST:
    {
      opus_int32 value = va_arg(ap, opus_int32);
      int max_supported = OPUS_FRAMESIZE_60_MS;
#ifdef OPUS_FRAMESIZE_120_MS
      max_supported = OPUS_FRAMESIZE_120_MS;
#endif
      if (value < OPUS_FRAMESIZE_2_5_MS || value > max_supported) {
        ret = OPUS_UNIMPLEMENTED;
        break;
      }
      ret = opus_multistream_encoder_ctl(enc->st, request, value);
      if (ret == OPUS_OK) {
        if (value <= OPUS_FRAMESIZE_40_MS)
          enc->frame_size = 120<<(value-OPUS_FRAMESIZE_2_5_MS);
        else
          enc->frame_size = (value-OPUS_FRAMESIZE_2_5_MS-2)*960;
      }
    }
    break;
    default:
      ret = OPUS_UNIMPLEMENTED;
  }
  va_end(ap);
  return ret;
}

/* ctl()-type call for the OggOpus layer. */
int ope_set_params(OggOpusEnc *enc, int request, ...) {
  int ret;
  va_list ap;
  va_start(ap, request);
  switch (request) {
    case OPE_SET_DECISION_DELAY_REQUEST:
    {
      opus_int32 value = va_arg(ap, opus_int32);
      if (value < 0) {
        ret = OPE_BAD_ARG;
        break;
      }
      enc->decision_delay = value;
      ret = OPE_OK;
    }
    break;
    case OPE_SET_MUXING_DELAY_REQUEST:
    {
      opus_int32 value = va_arg(ap, opus_int32);
      if (value < 0) {
        ret = OPE_BAD_ARG;
        break;
      }
      enc->max_ogg_delay = value;
      ret = OPE_OK;
    }
    break;
    case OPE_SET_COMMENT_PADDING_REQUEST:
    {
      opus_int32 value = va_arg(ap, opus_int32);
      if (value < 0) {
        ret = OPE_BAD_ARG;
        break;
      }
      enc->comment_padding = value;
      ret = OPE_OK;
    }
    break;
    case OPE_SET_SERIALNO_REQUEST:
    {
      opus_int32 value = va_arg(ap, opus_int32);
      if (enc->last_stream->header_is_frozen) return OPE_TOO_LATE;
      enc->last_stream->serialno = value;
      enc->last_stream->serialno_is_set = 1;
      ret = OPE_OK;
    }
    break;
    default:
      return OPE_UNIMPLEMENTED;
  }
  va_end(ap);
  return ret;
}
