#include <stdio.h>
#include "opusenc.h"

int main(int argc, char **argv) {
  FILE *fin;
  OggOpusEnc *enc;
  int error;
  if (argc != 3) {
    fprintf(stderr, "usage: %s <raw pcm input> <Ogg Opus output>\n", argv[0]);
    return 1;
  }
  fin = fopen(argv[1], "r");
  if (!fin) {
    printf("cannout open input file: %s\n", argv[1]);
    return 1;
  }
  enc = ope_create_file(argv[2], 48000, 2, 0, &error);
  if (!enc) {
    printf("cannout open output file: %s\n", argv[2]);
    return 1;
  }
  ope_add_comment(enc, "ARTIST", "Someone");
  ope_add_comment(enc, "TITLE", "Some track");
  ope_close_and_free(enc);
  return 0;
}
