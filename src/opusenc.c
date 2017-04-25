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

#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <opus_multistream.h>
#include "opusenc.h"
#include "opus_header.h"

#define BUFFER_SAMPLES 96000

struct StdioObject {
  FILE *file;
};

struct OggOpusEnc {
  OpusMSEncoder *st;
  float *buffer;
  OpusEncCallbacks callbacks;
  void *user_data;
  int os_allocated;
  ogg_stream_state os;
  ogg_page og;
  ogg_packet op;
};

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
OggOpusEnc *ope_create_file(const char *path, const OggOpusComments *comments,
    int rate, int channels, int family, int *error) {
  OggOpusEnc *enc;
  struct StdioObject *obj;
  obj = malloc(sizeof(*obj));
  enc = ope_create_callbacks(&stdio_callbacks, obj, comments, rate, channels, family, error);
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

/* Create a new OggOpus file (callback-based). */
OggOpusEnc *ope_create_callbacks(const OpusEncCallbacks *callbacks, void *user_data,
    const OggOpusComments *comments, int rate, int channels, int family, int *error) {
  OpusMSEncoder *st=NULL;
  OggOpusEnc *enc=NULL;
  OpusHeader header;
  int ret;
  if (family != 0 && family != 1 && family != 255) {
    if (error) *error = OPE_UNIMPLEMENTED;
    return NULL;
  }
  if (channels <= 0 || channels > 255) {
    if (error) *error = OPE_BAD_ARG;
    return NULL;
  }
  header.channels=channels;
  header.channel_mapping=family;
  header.input_sample_rate=rate;
  header.gain=0;
  st=opus_multistream_surround_encoder_create(48000, channels, header.channel_mapping, &header.nb_streams, &header.nb_coupled,
     header.stream_map, OPUS_APPLICATION_AUDIO, &ret);
  if (! (ret == OPUS_OK && st != NULL) ) {
    goto fail;
  }
  if ( (enc = malloc(sizeof(*enc))) == NULL) goto fail;
  enc->os_allocated = 0;
  if ( (enc->buffer = malloc(sizeof(*enc->buffer)*BUFFER_SAMPLES*channels)) == NULL) goto fail;
  enc->st = st;
  enc->callbacks = *callbacks;
  enc->user_data = user_data;
  (void)comments;
  return enc;
fail:
  if (enc) {
    free(enc);
    if (enc->buffer) free(enc->buffer);
  }
  if (st) {
    opus_multistream_encoder_destroy(st);
  }
  return NULL;
}

static void init_stream(OggOpusEnc *enc) {
  time_t start_time;
  int serialno;
  start_time = time(NULL);
  srand(((getpid()&65535)<<15)^start_time);

  serialno = rand();
  if (ogg_stream_init(&enc->os, serialno) == -1) {
    assert(0);
    /* FIXME: How the hell do we handle that? */
  }
}

/* Add/encode any number of float samples to the file. */
int ope_write_float(OggOpusEnc *enc, float *pcm, int samples_per_channel) {
  (void)enc;
  (void)pcm;
  (void)samples_per_channel;
  return 0;
}

/* Add/encode any number of int16 samples to the file. */
int ope_write(OggOpusEnc *enc, opus_int16 *pcm, int samples_per_channel) {
  (void)enc;
  (void)pcm;
  (void)samples_per_channel;
  return 0;
}

static void finalize_stream(OggOpusEnc *enc) {
  (void)enc;
}

/* Close/finalize the stream. */
int ope_close_and_free(OggOpusEnc *enc) {
  finalize_stream(enc);
  free(enc->buffer);
  opus_multistream_encoder_destroy(enc->st);
  if (enc->os_allocated) ogg_stream_clear(&enc->os);
  return OPE_OK;
}

/* Ends the stream and create a new stream within the same file. */
int ope_chain_current(OggOpusEnc *enc, const OggOpusComments *comments) {
  (void)enc;
  (void)comments;
  return 0;
}

/* Ends the stream and create a new file. */
int ope_continue_new_file(OggOpusEnc *enc, const OggOpusComments *comments, const char *path) {
  (void)enc;
  (void)comments;
  (void)path;
  return 0;
}

/* Ends the stream and create a new file (callback-based). */
int ope_continue_new_callbacks(OggOpusEnc *enc, const OggOpusComments *comments, void *user_data) {
  (void)enc;
  (void)comments;
  (void)user_data;
  return 0;
}

/* Goes straight to the libopus ctl() functions. */
int ope_encoder_ctl(OggOpusEnc *enc, int request, ...) {
  (void)enc;
  (void)request;
  return 0;
}

/* ctl()-type call for the OggOpus layer. */
int ope_set_params(OggOpusEnc *enc, int request, ...) {
  (void)enc;
  (void)request;
  return 0;
}
