/* Copyright (c) 2017 Jean-Marc Valin */
/*
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

#if !defined(_opusenc_h)
# define _opusenc_h (1)


# if defined(__cplusplus)
extern "C" {
# endif

#include <opus.h>

#ifndef OPE_EXPORT
# if defined(WIN32)
#  if defined(OPE_BUILD) && defined(DLL_EXPORT)
#   define OPE_EXPORT __declspec(dllexport)
#  else
#   define OPE_EXPORT
#  endif
# elif defined(__GNUC__) && defined(OPE_BUILD)
#  define OPE_EXPORT __attribute__ ((visibility ("default")))
# else
#  define OPE_EXPORT
# endif
#endif

/* Bump this when we change the API. */
/** API version for this header. Can be used to check for features at compile time. */
#define OPE_API_VERSION 0

#define OPE_OK 0
/* Based on the relevant libopus code minus 10. */
#define OPE_BAD_ARG -11
#define OPE_INTERNAL_ERROR -13
#define OPE_UNIMPLEMENTED -15
#define OPE_ALLOC_FAIL -17

/* Specific to libopusenc. */
#define OPE_CANNOT_OPEN -30
#define OPE_TOO_LATE -31
#define OPE_UNRECOVERABLE -32


/* These are the "raw" request values -- they should usually not be used. */
#define OPE_SET_DECISION_DELAY_REQUEST      14000
#define OPE_GET_DECISION_DELAY_REQUEST      14001
#define OPE_SET_MUXING_DELAY_REQUEST        14002
#define OPE_GET_MUXING_DELAY_REQUEST        14003
#define OPE_SET_COMMENT_PADDING_REQUEST     14004
#define OPE_GET_COMMENT_PADDING_REQUEST     14005
#define OPE_SET_SERIALNO_REQUEST            14006
#define OPE_GET_SERIALNO_REQUEST            14007
#define OPE_SET_PACKET_CALLBACK_REQUEST     14008
#define OPE_GET_PACKET_CALLBACK_REQUEST     14009

#define OPE_SET_DECISION_DELAY(x) OPE_SET_DECISION_DELAY_REQUEST, __opus_check_int(x)
#define OPE_GET_DECISION_DELAY(x) OPE_GET_DECISION_DELAY_REQUEST, __opus_check_int_ptr(x)
#define OPE_SET_MUXING_DELAY(x) OPE_SET_MUXING_DELAY_REQUEST, __opus_check_int(x)
#define OPE_GET_MUXING_DELAY(x) OPE_GET_MUXING_DELAY_REQUEST, __opus_check_int_ptr(x)
#define OPE_SET_COMMENT_PADDING(x) OPE_SET_COMMENT_PADDING_REQUEST, __opus_check_int(x)
#define OPE_GET_COMMENT_PADDING(x) OPE_GET_COMMENT_PADDING_REQUEST, __opus_check_int_ptr(x)
#define OPE_SET_SERIALNO(x) OPE_SET_SERIALNO_REQUEST, __opus_check_int(x)
#define OPE_GET_SERIALNO(x) OPE_GET_SERIALNO_REQUEST, __opus_check_int_ptr(x)
/* FIXME: Add type-checking macros to these. */
#define OPE_SET_PACKET_CALLBACK(x,u) OPE_SET_PACKET_CALLBACK_REQUEST, (x), (u)
#define OPE_GET_PACKET_CALLBACK(x,u) OPE_GET_PACKET_CALLBACK_REQUEST, (x), (u)

typedef int (*ope_write_func)(void *user_data, const unsigned char *ptr, opus_int32 len);

typedef int (*ope_close_func)(void *user_data);

typedef int (*ope_packet_func)(void *user_data, const unsigned char *packet_ptr, opus_int32 packet_len, opus_uint32 flags);

/** Callback functions for accessing the stream. */
typedef struct {
  /** Callback for writing to the stream. */
  ope_write_func write;
  /** Callback for closing the stream. */
  ope_close_func close;
} OpusEncCallbacks;

/** Opaque comments struct. */
typedef struct OggOpusComments OggOpusComments;

/** Opaque encoder struct. */
typedef struct OggOpusEnc OggOpusEnc;

/** Create a new comments object. */
OPE_EXPORT OggOpusComments *ope_comments_create(void);

/** Create a deep copy of a comments object. */
OPE_EXPORT OggOpusComments *ope_comments_copy(OggOpusComments *comments);

/** Destroys a comments object. */
OPE_EXPORT void ope_comments_destroy(OggOpusComments *comments);

/** Add a comment. */
OPE_EXPORT int ope_comments_add(OggOpusComments *comments, const char *tag, const char *val);

/** Add a comment. */
OPE_EXPORT int ope_comments_add_string(OggOpusComments *comments, const char *tag_and_val);

/** Add a picture. */
OPE_EXPORT int ope_comments_add_picture(OggOpusComments *comments, const char *spec);


/** Create a new OggOpus file. */
OPE_EXPORT OggOpusEnc *ope_encoder_create_file(const char *path, const OggOpusComments *comments, opus_int32 rate, int channels, int family, int *error);

/** Create a new OggOpus file (callback-based). */
OPE_EXPORT OggOpusEnc *ope_encoder_create_callbacks(const OpusEncCallbacks *callbacks, void *user_data,
    const OggOpusComments *comments, opus_int32 rate, int channels, int family, int *error);

/** Create a new OggOpus stream, pulling one page at a time. */
OPE_EXPORT OggOpusEnc *ope_encoder_create_pull(const OggOpusComments *comments, opus_int32 rate, int channels, int family, int *error);

/** Add/encode any number of float samples to the file. */
OPE_EXPORT int ope_encoder_write_float(OggOpusEnc *enc, const float *pcm, int samples_per_channel);

/** Add/encode any number of int16 samples to the file. */
OPE_EXPORT int ope_encoder_write(OggOpusEnc *enc, const opus_int16 *pcm, int samples_per_channel);

/** Get the next page from the stream. Returns 1 if there is a page available, 0 if not. */
OPE_EXPORT int ope_encoder_get_page(OggOpusEnc *enc, unsigned char **page, opus_int32 *len, int flush);

/** Finalizes the stream, but does not deallocate the object. */
OPE_EXPORT int ope_encoder_drain(OggOpusEnc *enc);

/** Deallocates the obect. Make sure to ope_drain() first. */
OPE_EXPORT void ope_encoder_destroy(OggOpusEnc *enc);

/** Ends the stream and create a new stream within the same file. */
OPE_EXPORT int ope_encoder_chain_current(OggOpusEnc *enc, const OggOpusComments *comments);

/** Ends the stream and create a new file. */
OPE_EXPORT int ope_encoder_continue_new_file(OggOpusEnc *enc, const char *path, const OggOpusComments *comments);

/** Ends the stream and create a new file (callback-based). */
OPE_EXPORT int ope_encoder_continue_new_callbacks(OggOpusEnc *enc, void *user_data, const OggOpusComments *comments);

/** Write out the header now rather than wait for audio to begin. */
OPE_EXPORT int ope_encoder_flush_header(OggOpusEnc *enc);

/** Goes straight to the libopus ctl() functions. */
OPE_EXPORT int ope_encoder_ctl(OggOpusEnc *enc, int request, ...);

/** Returns a string representing the version of libopusenc being used at run time. */
OPE_EXPORT const char *ope_get_version_string(void);

/** ABI version for this header. Can be used to check for features at run time. */
OPE_EXPORT int ope_get_abi_version(void);

# if defined(__cplusplus)
}
# endif

#endif
