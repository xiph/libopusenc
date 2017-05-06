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
#include "ogg_packer.h"

#define MAX_CHANNELS 8

#define LPC_PADDING 120
#define LPC_ORDER 24
#define LPC_INPUT 480

/* Allow up to 2 seconds for delayed decision. */
#define MAX_LOOKAHEAD 96000
/* We can't have a circular buffer (because of delayed decision), so let's not copy too often. */
#define BUFFER_EXTRA 24000

#define BUFFER_SAMPLES (MAX_LOOKAHEAD + BUFFER_EXTRA)

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define MAX_PACKET_SIZE (1276*8)

#define USE_OGGP

struct StdioObject {
  FILE *file;
};

typedef struct EncStream EncStream;

struct EncStream {
  void *user_data;
#ifndef USE_OGGP
  ogg_stream_state os;
#endif
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
  ogg_int64_t granule_offset;
  EncStream *next;
};

struct OggOpusEnc {
  OpusMSEncoder *st;
#ifdef USE_OGGP
  oggpacker *oggp;
#endif
  int unrecoverable;
  int pull_api;
  int rate;
  int channels;
  float *buffer;
  int buffer_start;
  int buffer_end;
  SpeexResamplerState *re;
  int frame_size;
  int decision_delay;
  int max_ogg_delay;
  int global_granule_offset;
  ogg_int64_t curr_granule;
  ogg_int64_t write_granule;
  ogg_int64_t last_page_granule;
  unsigned char *chaining_keyframe;
  int chaining_keyframe_length;
  OpusEncCallbacks callbacks;
  ope_packet_func packet_callback;
  OpusHeader header;
  int comment_padding;
  EncStream *streams;
  EncStream *last_stream;
};

#ifdef USE_OGGP
static void output_pages(OggOpusEnc *enc) {
  unsigned char *page;
  int len;
  while (oggp_get_next_page(enc->oggp, &page, &len)) {
    enc->callbacks.write(enc->streams->user_data, page, len);
  }
}
static void oe_flush_page(OggOpusEnc *enc) {
  oggp_flush_page(enc->oggp);
  if (!enc->pull_api) output_pages(enc);
}

#else

static int oe_write_page(OggOpusEnc *enc, ogg_page *page, void *user_data)
{
  int length;
  int err;
  err = enc->callbacks.write(user_data, page->header, page->header_len);
  if (err) return -1;
  err = enc->callbacks.write(user_data, page->body, page->body_len);
  if (err) return -1;
  length = page->header_len+page->body_len;
  return length;
}

static void oe_flush_page(OggOpusEnc *enc) {
  ogg_page og;
  int ret;

  while ( (ret = ogg_stream_flush_fill(&enc->streams->os, &og, 255*255))) {
    if (ogg_page_packets(&og) != 0) enc->last_page_granule = ogg_page_granulepos(&og) + enc->streams->granule_offset;
    ret = oe_write_page(enc, &og, enc->streams->user_data);
    if (ret == -1) {
      return;
    }
  }
}
#endif


int stdio_write(void *user_data, const unsigned char *ptr, int len) {
  struct StdioObject *obj = (struct StdioObject*)user_data;
  return fwrite(ptr, 1, len, obj->file) != (size_t)len;
}

int stdio_close(void *user_data) {
  struct StdioObject *obj = (struct StdioObject*)user_data;
  int ret = 0;
  if (obj->file) ret = fclose(obj->file);
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
    ope_destroy(enc);
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
  stream->granule_offset = 0;
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
#ifndef USE_OGGP
  if (stream->stream_is_init) ogg_stream_clear(&stream->os);
#endif
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
  if (rate <= 0) {
    if (error) *error = OPE_BAD_ARG;
    return NULL;
  }

  if ( (enc = malloc(sizeof(*enc))) == NULL) goto fail;
  enc->streams = NULL;
  if ( (enc->streams = stream_create()) == NULL) goto fail;
  enc->streams->next = NULL;
  enc->last_stream = enc->streams;
#ifdef USE_OGGP
  enc->oggp = NULL;
#endif
  enc->unrecoverable = 0;
  enc->pull_api = 0;
  enc->packet_callback = NULL;
  enc->rate = rate;
  enc->channels = channels;
  enc->frame_size = 960;
  enc->decision_delay = 96000;
  enc->max_ogg_delay = 48000;
  enc->chaining_keyframe = NULL;
  enc->chaining_keyframe_length = -1;
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
    enc->global_granule_offset = enc->header.preskip;
  }
  enc->curr_granule = 0;
  enc->write_granule = 0;
  enc->last_page_granule = 0;
  if ( (enc->buffer = malloc(sizeof(*enc->buffer)*BUFFER_SAMPLES*channels)) == NULL) goto fail;
  enc->buffer_start = enc->buffer_end = 0;
  enc->st = st;
  enc->callbacks = *callbacks;
  enc->streams->user_data = user_data;
  if (error) *error = OPE_OK;
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
  if (error) *error = OPE_ALLOC_FAIL;
  return NULL;
}

/* Create a new OggOpus stream, pulling one page at a time. */
OPE_EXPORT OggOpusEnc *ope_create_pull(int rate, int channels, int family, int *error) {
  OggOpusEnc *enc = ope_create_callbacks(NULL, NULL, rate, channels, family, error);
  enc->pull_api = 1;
  return enc;
}

static void init_stream(OggOpusEnc *enc) {
  assert(!enc->streams->stream_is_init);
  if (!enc->streams->serialno_is_set) {
    enc->streams->serialno = rand();
  }

#ifdef USE_OGGP
  if (enc->oggp != NULL) oggp_chain(enc->oggp, enc->streams->serialno);
  else {
    enc->oggp = oggp_create(enc->streams->serialno);
    if (enc->oggp == NULL) {
      enc->unrecoverable = 1;
      return;
    }
    oggp_set_muxing_delay(enc->oggp, enc->max_ogg_delay);
  }
#else
  if (ogg_stream_init(&enc->streams->os, enc->streams->serialno) == -1) {
    enc->unrecoverable = 1;
    return;
  }
#endif
  comment_pad(&enc->streams->comment, &enc->streams->comment_length, enc->comment_padding);

  /*Write header*/
  {
#ifdef USE_OGGP
    unsigned char *p;
    p = oggp_get_packet_buffer(enc->oggp, 276);
    int packet_size = opus_header_to_packet(&enc->header, p, 276);
    oggp_commit_packet(enc->oggp, packet_size, 0, 0);
    oe_flush_page(enc);
    p = oggp_get_packet_buffer(enc->oggp, enc->streams->comment_length);
    memcpy(p, enc->streams->comment, enc->streams->comment_length);
    oggp_commit_packet(enc->oggp, enc->streams->comment_length, 0, 0);
    oe_flush_page(enc);

#else

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
#endif
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
  ogg_int64_t end_granule48k = (enc->streams->end_granule*48000 + enc->rate - 1)/enc->rate + enc->global_granule_offset;
  while (enc->buffer_end-enc->buffer_start > enc->frame_size + enc->decision_delay) {
    int cont;
    opus_int32 pred;
    int flush_needed;
    ogg_packet op;
#ifndef USE_OGGP
    ogg_page og;
#endif
    int nbBytes;
    unsigned char packet[MAX_PACKET_SIZE];
    int is_keyframe=0;
    if (enc->unrecoverable) return;
    opus_multistream_encoder_ctl(enc->st, OPUS_GET_PREDICTION_DISABLED(&pred));
    /* FIXME: a frame that follows a keyframe generally doesn't need to be a keyframe
       unless there's two consecutive stream boundaries. */
    if (enc->curr_granule + 2*enc->frame_size>= end_granule48k && enc->streams->next) {
      opus_multistream_encoder_ctl(enc->st, OPUS_SET_PREDICTION_DISABLED(1));
      is_keyframe = 1;
    }
    nbBytes = opus_multistream_encode_float(enc->st, &enc->buffer[enc->channels*enc->buffer_start],
        enc->buffer_end-enc->buffer_start, packet, MAX_PACKET_SIZE);
    if (nbBytes < 0) {
      /* Anything better we can do here? */
      enc->unrecoverable = 1;
      return;
    }
    opus_multistream_encoder_ctl(enc->st, OPUS_SET_PREDICTION_DISABLED(pred));
    assert(nbBytes > 0);
    enc->curr_granule += enc->frame_size;
    op.packet=packet;
    op.bytes=nbBytes;
    op.b_o_s=0;
    op.packetno=enc->streams->packetno++;
    do {
      op.granulepos=enc->curr_granule-enc->streams->granule_offset;
      op.e_o_s=enc->curr_granule >= end_granule48k;
      cont = 0;
      if (op.e_o_s) op.granulepos=end_granule48k-enc->streams->granule_offset;
#ifdef USE_OGGP
      {
        unsigned char *p;
        p = oggp_get_packet_buffer(enc->oggp, MAX_PACKET_SIZE);
        memcpy(p, packet, nbBytes);
        oggp_commit_packet(enc->oggp, nbBytes, op.granulepos, op.e_o_s);
      }
#else
      ogg_stream_packetin(&enc->streams->os, &op);
#endif
      if (enc->packet_callback) enc->packet_callback(enc->streams->user_data, op.packet, op.bytes, 0);
      /* FIXME: Also flush on too many segments. */
#ifdef USE_OGGP
      flush_needed = op.e_o_s;
      if (flush_needed) oe_flush_page(enc);
      else if (!enc->pull_api) output_pages(enc);
#else
      flush_needed = op.e_o_s || enc->curr_granule - enc->last_page_granule >= enc->max_ogg_delay;
      if (flush_needed) {
        oe_flush_page(enc);
      } else {
        while (ogg_stream_pageout_fill(&enc->streams->os, &og, 255*255)) {
          if (ogg_page_packets(&og) != 0) enc->last_page_granule = ogg_page_granulepos(&og) + enc->streams->granule_offset;
          int ret = oe_write_page(enc, &og, enc->streams->user_data);
          /* FIXME: what do we do if this fails? */
          assert(ret != -1);
        }
      }
#endif
      if (op.e_o_s) {
        EncStream *tmp;
        tmp = enc->streams->next;
        if (enc->streams->close_at_end) enc->callbacks.close(enc->streams->user_data);
        stream_destroy(enc->streams);
        enc->streams = tmp;
        if (!tmp) enc->last_stream = NULL;
        if (enc->last_stream == NULL) return;
        /* We're done with this stream, start the next one. */
        enc->header.preskip = end_granule48k + enc->frame_size - enc->curr_granule;
        enc->streams->granule_offset = enc->curr_granule - enc->frame_size;
        if (enc->chaining_keyframe) {
          enc->header.preskip += enc->frame_size;
          enc->streams->granule_offset -= enc->frame_size;
        }
        init_stream(enc);
        if (enc->chaining_keyframe) {
          ogg_packet op2;
          op2.packet = enc->chaining_keyframe;
          op2.bytes = enc->chaining_keyframe_length;
          op2.b_o_s = 0;
          op2.e_o_s = 0;
          op2.packetno=enc->streams->packetno++;
          op2.granulepos=enc->curr_granule - enc->streams->granule_offset - enc->frame_size;
#ifdef USE_OGGP
          {
            unsigned char *p;
            p = oggp_get_packet_buffer(enc->oggp, MAX_PACKET_SIZE);
            memcpy(p, enc->chaining_keyframe, enc->chaining_keyframe_length);
            oggp_commit_packet(enc->oggp, enc->chaining_keyframe_length, op2.granulepos, 0);
          }
#else
          ogg_stream_packetin(&enc->streams->os, &op2);
#endif
          if (enc->packet_callback) enc->packet_callback(enc->streams->user_data, op2.packet, op2.bytes, 0);
        }
        end_granule48k = (enc->streams->end_granule*48000 + enc->rate - 1)/enc->rate + enc->global_granule_offset;
        cont = 1;
      }
    } while (cont);
    if (enc->chaining_keyframe) free(enc->chaining_keyframe);
    if (is_keyframe) {
      enc->chaining_keyframe = malloc(nbBytes);
      enc->chaining_keyframe_length = nbBytes;
      memcpy(enc->chaining_keyframe, packet, nbBytes);
    } else {
      enc->chaining_keyframe = NULL;
      enc->chaining_keyframe_length = -1;
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
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
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
    if (enc->unrecoverable) return OPE_UNRECOVERABLE;
  } while (samples_per_channel > 0);
  return OPE_OK;
}

#define CONVERT_BUFFER 256

/* Add/encode any number of int16 samples to the file. */
int ope_write(OggOpusEnc *enc, const opus_int16 *pcm, int samples_per_channel) {
  int channels = enc->channels;
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
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
    if (enc->unrecoverable) return OPE_UNRECOVERABLE;
  } while (samples_per_channel > 0);
  return OPE_OK;
}

/* Get the next page from the stream. Returns 1 if there is a page available, 0 if not. */
OPE_EXPORT int ope_get_page(OggOpusEnc *enc, unsigned char **page, int *len, int flush) {
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
  if (!enc->pull_api) return 0;
  else {
    if (flush) oggp_flush_page(enc->oggp);
    return oggp_get_next_page(enc->oggp, page, len);
  }
}

static void extend_signal(float *x, int before, int after, int channels);

int ope_drain(OggOpusEnc *enc) {
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
  /* Check if it's already been drained. */
  if (enc->streams == NULL) return OPE_TOO_LATE;
  /* FIXME: Use a better value. */
  int pad_samples = 3000;
  if (!enc->streams->stream_is_init) init_stream(enc);
  shift_buffer(enc);
  /* FIXME: Do LPC extension instead. */
  memset(&enc->buffer[enc->channels*enc->buffer_end], 0, pad_samples*enc->channels*sizeof(enc->buffer[0]));
  extend_signal(&enc->buffer[enc->channels*enc->buffer_end], enc->buffer_end, LPC_PADDING, enc->channels);
  enc->decision_delay = 0;
  enc->buffer_end += pad_samples;
  assert(enc->buffer_end <= BUFFER_SAMPLES);
  encode_buffer(enc);
  assert(enc->streams == NULL);
  return OPE_OK;
}

void ope_destroy(OggOpusEnc *enc) {
  EncStream *stream;
  stream = enc->streams;
  while (stream != NULL) {
    EncStream *tmp = stream;
    stream = stream->next;
    if (tmp->close_at_end) enc->callbacks.close(tmp->user_data);
    stream_destroy(tmp);
  }
  if (enc->chaining_keyframe) free(enc->chaining_keyframe);
  free(enc->buffer);
#ifdef USE_OGGP
  if (enc->oggp) oggp_destroy(enc->oggp);
#endif
  opus_multistream_encoder_destroy(enc->st);
  if (enc->re) speex_resampler_destroy(enc->re);
  free(enc);
}

/* Ends the stream and create a new stream within the same file. */
int ope_chain_current(OggOpusEnc *enc) {
  enc->last_stream->close_at_end = 0;
  return ope_continue_new_callbacks(enc, enc->last_stream->user_data);
}

/* Ends the stream and create a new file. */
int ope_continue_new_file(OggOpusEnc *enc, const char *path) {
  int ret;
  struct StdioObject *obj;
  if (!(obj = malloc(sizeof(*obj)))) return OPE_ALLOC_FAIL;
  obj->file = fopen(path, "wb");
  if (!obj->file) {
    free(obj);
    /* By trying to open the file first, we can recover if we can't open it. */
    return OPE_CANNOT_OPEN;
  }
  ret = ope_continue_new_callbacks(enc, obj);
  if (ret == OPE_OK) return ret;
  fclose(obj->file);
  free(obj);
  return ret;
}

/* Ends the stream and create a new file (callback-based). */
int ope_continue_new_callbacks(OggOpusEnc *enc, void *user_data) {
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
  EncStream *new_stream;
  assert(enc->streams);
  assert(enc->last_stream);
  new_stream = stream_create();
  if (!new_stream) return OPE_ALLOC_FAIL;
  new_stream->user_data = user_data;
  enc->last_stream->next = new_stream;
  enc->last_stream = new_stream;
  return OPE_OK;
}

/* Add a comment to the file (can only be called before encoding samples). */
int ope_add_comment(OggOpusEnc *enc, const char *tag, const char *val) {
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
  if (enc->last_stream->header_is_frozen) return OPE_TOO_LATE;
  if (enc->last_stream->stream_is_init) return OPE_TOO_LATE;
  if (comment_add(&enc->last_stream->comment, &enc->last_stream->comment_length, tag, val)) return OPE_ALLOC_FAIL;
  return OPE_OK;
}

int ope_add_picture(OggOpusEnc *enc, const char *spec) {
  const char *error_message;
  char *picture_data;
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
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
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
  if (enc->last_stream->header_is_frozen) return OPE_TOO_LATE;
  if (enc->last_stream->stream_is_init) return OPE_TOO_LATE;
  if (comment_replace_vendor_string(&enc->last_stream->comment, &enc->last_stream->comment_length, vendor)) return OPE_ALLOC_FAIL;
  return OPE_OK;
}

int ope_flush_header(OggOpusEnc *enc) {
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
  if (enc->last_stream->header_is_frozen) return OPE_TOO_LATE;
  if (enc->last_stream->stream_is_init) return OPE_TOO_LATE;
  else init_stream(enc);
  return OPE_OK;
}

/* Goes straight to the libopus ctl() functions. */
int ope_encoder_ctl(OggOpusEnc *enc, int request, ...) {
  int ret;
  int translate;
  va_list ap;
  if (enc->unrecoverable) return OPE_UNRECOVERABLE;
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
    case OPUS_MULTISTREAM_GET_ENCODER_STATE_REQUEST:
    {
      opus_int32 stream_id;
      OpusEncoder **value;
      stream_id = va_arg(ap, opus_int32);
      value = va_arg(ap, OpusEncoder**);
      ret = opus_multistream_encoder_ctl(enc->st, request, stream_id, value);
    }
    break;

    /* ****************** libopusenc-specific requests. ********************** */
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
#ifdef USE_OGGP
      oggp_set_muxing_delay(enc->oggp, enc->max_ogg_delay);
#endif
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
    case OPE_SET_PACKET_CALLBACK_REQUEST:
    {
      ope_packet_func value = va_arg(ap, ope_packet_func);
      enc->packet_callback = value;
      ret = OPE_OK;
    }
    break;
    default:
      return OPE_UNIMPLEMENTED;
  }
  va_end(ap);
  translate = request < 14000 || (ret < 0 && ret >= -10);
  if (translate) {
    if (ret == OPUS_BAD_ARG) ret = OPE_BAD_ARG;
    else if (ret == OPUS_INTERNAL_ERROR) ret = OPE_INTERNAL_ERROR;
    else if (ret == OPUS_UNIMPLEMENTED) ret = OPE_UNIMPLEMENTED;
    else if (ret == OPUS_ALLOC_FAIL) ret = OPE_ALLOC_FAIL;
    else ret = OPE_INTERNAL_ERROR;
  }
  assert(ret == 0 || ret < -10);
  return ret;
}


static void vorbis_lpc_from_data(float *data, float *lpci, int n, int stride);

static void extend_signal(float *x, int before, int after, int channels) {
  int c;
  before = MIN(before, LPC_INPUT);
  if (before < 4*LPC_ORDER) {
    int i;
    for (i=0;i<after*channels;i++) x[i] = 0;
    return;
  }
  for (c=0;c<channels;c++) {
    int i;
    float lpc[LPC_ORDER];
    vorbis_lpc_from_data(x-channels*before, lpc, before, channels);
    for (i=0;i<after;i++) {
      float sum;
      int j;
      sum = 0;
      for (j=0;j<LPC_ORDER;j++) sum -= x[i-j-1]*lpc[j];
      x[i] = sum;
    }
  }
}

/* Some of these routines (autocorrelator, LPC coefficient estimator)
   are derived from code written by Jutta Degener and Carsten Bormann;
   thus we include their copyright below.  The entirety of this file
   is freely redistributable on the condition that both of these
   copyright notices are preserved without modification.  */

/* Preserved Copyright: *********************************************/

/* Copyright 1992, 1993, 1994 by Jutta Degener and Carsten Bormann,
Technische Universita"t Berlin

Any use of this software is permitted provided that this notice is not
removed and that neither the authors nor the Technische Universita"t
Berlin are deemed to have made any representations as to the
suitability of this software for any purpose nor are held responsible
for any defects of this software. THERE IS ABSOLUTELY NO WARRANTY FOR
THIS SOFTWARE.

As a matter of courtesy, the authors request to be informed about uses
this software has found, about bugs in this software, and about any
improvements that may be of general interest.

Berlin, 28.11.1994
Jutta Degener
Carsten Bormann

*********************************************************************/

static void vorbis_lpc_from_data(float *data, float *lpci, int n, int stride) {
  double aut[LPC_ORDER+1];
  double lpc[LPC_ORDER];
  double error;
  double epsilon;
  int i,j;

  /* FIXME: Apply a window to the input. */
  /* autocorrelation, p+1 lag coefficients */
  j=LPC_ORDER+1;
  while(j--){
    double d=0; /* double needed for accumulator depth */
    for(i=j;i<n;i++)d+=(double)data[i*stride]*data[(i-j)*stride];
    aut[j]=d;
  }

  /* FIXME: Apply lag windowing (better than bandwidth expansion) */
  /* Generate lpc coefficients from autocorr values */

  /* FIXME: This noise floor is insane! */
  /* set our noise floor to about -100dB */
  error=aut[0] * (1. + 1e-10);
  epsilon=1e-9*aut[0]+1e-10;

  for(i=0;i<LPC_ORDER;i++){
    double r= -aut[i+1];

    if(error<epsilon){
      memset(lpc+i,0,(LPC_ORDER-i)*sizeof(*lpc));
      goto done;
    }

    /* Sum up this iteration's reflection coefficient; note that in
       Vorbis we don't save it.  If anyone wants to recycle this code
       and needs reflection coefficients, save the results of 'r' from
       each iteration. */

    for(j=0;j<i;j++)r-=lpc[j]*aut[i-j];
    r/=error;

    /* Update LPC coefficients and total error */

    lpc[i]=r;
    for(j=0;j<i/2;j++){
      double tmp=lpc[j];

      lpc[j]+=r*lpc[i-1-j];
      lpc[i-1-j]+=r*tmp;
    }
    if(i&1)lpc[j]+=lpc[j]*r;

    error*=1.-r*r;

  }

 done:

  /* slightly damp the filter */
  {
    double g = .99;
    double damp = g;
    for(j=0;j<LPC_ORDER;j++){
      lpc[j]*=damp;
      damp*=g;
    }
  }

  for(j=0;j<LPC_ORDER;j++)lpci[j]=(float)lpc[j];
}

