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

#include <stdlib.h>
#include "opusenc.h"

/* Create a new OggOpus file. */
OggOpusEnc *ope_create_file(const char *path, const OggOpusComments *comments,
  int rate, int channels, int family, int *error) {
  return NULL;
}

/* Create a new OggOpus file (callback-based). */
OggOpusEnc *ope_create_callbacks(OpusEncCallbacks *callbacks, const OggOpusComments *comments,
  void *user_data, int rate, int channels, int family, int *error) {
  return NULL;
}

/* Add/encode any number of float samples to the file. */
int ope_write_float(OggOpusEnc *enc, float *pcm, int samples_per_channel) {
  return 0;
}

/* Add/encode any number of int16 samples to the file. */
int ope_write(OggOpusEnc *enc, opus_int16 *pcm, int samples_per_channel) {
  return 0;
}

/* Close/finalize the stream. */
int ope_close_and_free(OggOpusEnc *enc) {
  return 0;
}

/* Ends the stream and create a new stream within the same file. */
int ope_chain_current(OggOpusEnc *enc) {
  return 0;
}

/* Ends the stream and create a new file. */
int ope_continue_new_file(OggOpusEnc *enc, const OggOpusComments *comments, const char *path) {
  return 0;
}

/* Ends the stream and create a new file (callback-based). */
int ope_continue_new_callbacks(OggOpusEnc *enc, const OggOpusComments *comments, void *user_data) {
  return 0;
}

/* Goes straight to the libopus ctl() functions. */
int ope_encoder_ctl(OggOpusEnc *enc, int request, ...) {
  return 0;
}

/* ctl()-type call for the OggOpus layer. */
int ope_set_params(OggOpusEnc *enc, int request, ...) {
  return 0;
}
