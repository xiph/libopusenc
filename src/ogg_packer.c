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
#include <string.h>
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
  int buf_begin;
  unsigned char *lacing;
  int lacing_size;
  int lacing_fill;
  int lacing_begin;
  oggp_page *pages;
  int pages_size;
  int pages_fill;
  int muxing_delay;
  int is_eos;
  oggp_uint64 curr_granule;
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
  oggp->buf_begin = 0;
  oggp->lacing_fill = 0;
  oggp->lacing_begin = 0;
  oggp->pages_fill = 0;

  oggp->is_eos = 0;
  oggp->curr_granule = 0;
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
unsigned char *oggp_get_packet_buffer(oggpacker *oggp, int bytes) {
  if (oggp->buf_fill + bytes > oggp->buf_size) {
    /* FIXME: make room somehow. */
  }
  return &oggp->buf[oggp->buf_fill];
}

/** Tells the oggpacker that the packet buffer obtained from
    oggp_get_packet_buffer() has been filled and the number of bytes written
    has to be no more than what was originally asked for. */
int oggp_commit_packet(oggpacker *oggp, int bytes, oggp_uint64 granulepos, int eos) {
  int i;
  int nb_255s;
  oggp->buf_fill += bytes;
  nb_255s = bytes/255;
  /* FIXME: Check if we should flush before the packet. */
  /* FIXME: Check lacing size */
  for (i=0;i<nb_255s;i++) {
    oggp->lacing[oggp->lacing_fill+i] = 255;
  }
  oggp->lacing[oggp->lacing_fill+nb_255s] = bytes - 255*nb_255s;
  oggp->curr_granule = granulepos;
  oggp->is_eos = eos;
  /* FIXME: Check if we should flush after the packet. */
  return 0;
}

/** Create a page from the data written so far (and not yet part of a previous page).
    If there is too much data for one page, then either:
    1) all page continuations will be closed too (close_cont=1)
    2) all but the last page continuations will be closed (close_cont=0)*/
int oggp_flush_page(oggpacker *oggp, int close_cont) {
  oggp_page *p;
  /* FIXME: Check we have a free page. */
  /* FIXME: Check there is at least one packet. */
  p = &oggp->pages[oggp->pages_fill++];
  p->granulepos = oggp->curr_granule;

  p->buf_pos = oggp->buf_begin;
  p->buf_size = oggp->buf_fill - oggp->buf_begin;
  p->lacing_pos = oggp->lacing_begin;
  p->lacing_size = oggp->lacing_fill - oggp->lacing_begin;
  /* FIXME: Handle bos/eos and continued pages. */
  p->flags = 0;
  (void)close_cont;
  return 0;
}

/** Get a pointer to the contents of the next available page. Pointer is
    invalidated on the next call to oggp_get_next_page(). */
int oggp_get_next_page(oggpacker *oggp, unsigned char **page, int *bytes) {
  oggp_page *p;
  unsigned char *ptr;
  int header_size;
  p = &oggp->pages[0];
  header_size = 27 + p->lacing_size;
  ptr = &oggp->buf[p->buf_pos - header_size];
  memcpy(&ptr[27], &oggp->lacing[p->lacing_pos], p->lacing_size);
  memcpy(ptr, "OggS", 4);
  /* FIXME: Write the other fields. */

  *page = ptr;
  *bytes = p->buf_size + header_size;
  oggp->pages_fill--;
  memmove(&oggp->pages[0], &oggp->pages[1], oggp->pages_fill);
  return 0;
}

/** Creates a new (chained) stream. This closes all outstanding pages. These
    pages remain available with oggp_get_next_page(). */
int oggp_chain(oggpacker *oggp, int serialno) {
  oggp_flush_page(oggp, 1);
  oggp->serialno = serialno;
  oggp->curr_granule = 0;
  oggp->is_eos = 0;
  return 0;
}
