

#if !defined(_opusenc_h)
# define _opusenc_h (1)


# if defined(__cplusplus)
extern "C" {
# endif

typedef struct {
  ope_write_func write;
  ope_close_func close;
} OpusEncCallbacks;

/* Opaque encoder struct. */
typedef struct OggOpusEnc OggOpusEnc;

/* Opaque header struct. */
typedef struct OggOpusComments OggOpusComments;

/* Create a new OggOpus file. */
OggOpusEnc *ope_create_file(const char *path, int rate, int channels, int family, int *error);

/* Create a new OggOpus file (callback-based). */
OggOpusEnc *ope_create_callbacks(OpusEncCallbacks *callbacks, void *user_data, int rate, int channels, int family, int *error);

/* Add/encode any number of float samples to the file. */
int ope_write_float(OggOpusEnc *enc, float *pcm, int samples_per_channel);

/* Add/encode any number of int16 samples to the file. */
int ope_write(OggOpusEnc *enc, opus_int16 *pcm, int samples_per_channel);

/* Close/finalize the stream. */
int ope_free(OggOpusEnc *enc);

/* Ends the stream and create a new stream within the same file. */
int ope_chain_current(OggOpusEnc *enc);

/* Ends the stream and create a new file. */
int ope_chain_new_file(OggOpusEnc *enc, const char *path);

/* Ends the stream and create a new file (callback-based). */
int ope_chain_new_callbacks(OggOpusEnc *enc, void *user_data);

/* Goes straight to the libopus ctl() functions. */
int ope_encoder_ctl(OggOpusEnc *enc, int request, ...);

/* ctl()-type call for the OggOpus layer. */
int ope_set_params(OggOpusEnc *enc, int request, ...);

# if defined(__cplusplus)
}
# endif

#endif
