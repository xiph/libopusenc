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

#include <stdlib.h>
#include "ogg_packer.h"

#define MAX_HEADER_SIZE (27+255)

#define MAX_PAGE_SIZE (255*255 + MAX_HEADER_SIZE)

typedef struct {
  oggp_uint64 granulepos;
  int buf_pos;
  int buf_size;
  int lacing_pos;
  int lacing_size;
  int flags;
} oggp_page;

struct oggpacker {
  int serialno;
  unsigned char *buf;
  unsigned char *alloc_buf;
  int buf_size;
  int buf_fill;
  unsigned char *lacing;
  int lacing_size;
  int lacing_fill;
  oggp_page *pages;
  int pages_size;
  int pages_fill;
  int muxing_delay;
};

/** Allocates an oggpacker object */
oggpacker *oggp_create(int serialno) {
  oggpacker *oggp;
  oggp = malloc(sizeof(*oggp));
  if (oggp == NULL) goto fail;
  oggp->alloc_buf = NULL;
  oggp->lacing = NULL;
  oggp->pages = NULL;

  oggp->buf_size = MAX_PAGE_SIZE;
  oggp->lacing_size = 256;
  oggp->pages_size = 10;

  oggp->alloc_buf = malloc(oggp->buf_size);
  oggp->lacing = malloc(oggp->lacing_size);
  oggp->pages = malloc(oggp->pages_size * sizeof(oggp->pages[0]));
  if (!oggp->alloc_buf || !oggp->lacing || !oggp->pages) goto fail;
  oggp->buf = oggp->alloc_buf + MAX_HEADER_SIZE;

  oggp->serialno = serialno;
  oggp->buf_fill = 0;
  oggp->lacing_fill = 0;
  oggp->pages_fill = 0;
fail:
  if (oggp) {
    if (oggp->lacing) free(oggp->lacing);
    if (oggp->alloc_buf) free(oggp->alloc_buf);
    if (oggp->pages) free(oggp->pages);
    free(oggp);
  }
  return NULL;
}

/** Frees memory associated with an oggpacker object */
void oggp_destroy(oggpacker *oggp) {
  free(oggp->lacing);
  free(oggp->alloc_buf);
  free(oggp->pages);
  free(oggp);
}

/** Sets the maximum muxing delay in granulepos units. Pages will be auto-flushed
    to enforce the delay and to avoid continued pages if possible. */
void oggp_set_muxing_delay(oggpacker *oggp, oggp_uint64 delay) {
  oggp->muxing_delay = delay;
}

/** Get a buffer where to write the next packet. The buffer will have
    size "bytes", but fewer bytes can be written. The buffer remains valid through
    a call to oggp_close_page() or oggp_get_next_page(), but is invalidated by
    another call to oggp_get_packet_buffer() or by a call to oggp_commit_packet(). */
unsigned char *oggp_get_packet_buffer(oggpacker *oggp, int bytes);

/** Tells the oggpacker that the packet buffer obtained from
    oggp_get_packet_buffer() has been filled and the number of bytes written
    has to be no more than what was originally asked for. */
int oggp_commit_packet(oggpacker *oggp, int bytes, oggp_uint64 granulepos, int eos);

/** Create a page from the data written so far (and not yet part of a previous page).
    If there is too much data for one page, then either:
    1) all page continuations will be closed too (close_cont=1)
    2) all but the last page continuations will be closed (close_cont=0)*/
int oggp_flush_page(oggpacker *oggp, int close_cont);

/** Get a pointer to the contents of the next available page. Pointer is
    invalidated on the next call to oggp_get_next_page(). */
int oggp_get_next_page(oggpacker *oggp, unsigned char **page, int *bytes);

/** Creates a new (chained) stream. This closes all outstanding pages. These
    pages remain available with oggp_get_next_page(). */
int oggp_chain(oggpacker *oggp, int serialno);
