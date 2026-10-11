#include <string.h>

#include "fairplay.h"
#include "fairplay_tables.h"
#include "playfair/playfair.h"

#define FAIRPLAY_VERSION 0x03

int fairplay_setup(fairplay_t* fp, const unsigned char req[16],
                   unsigned char res[142]) {
  unsigned char mode;

  if (req[4] != FAIRPLAY_VERSION) return -1;

  // UxPlay indexes the table with this byte unchecked.  It arrives off the
  // network, so bound it here rather than read past the end of the table.
  mode = req[14];
  if (mode > 3) return -1;

  memcpy(res, reply_message[mode], 142);
  fp->keymsglen = 0;  // a new stage 1 invalidates any stored stage 2
  return 0;
}

int fairplay_handshake(fairplay_t* fp, const unsigned char req[164],
                       unsigned char res[32]) {
  if (req[4] != FAIRPLAY_VERSION) return -1;

  memcpy(fp->keymsg, req, 164);
  fp->keymsglen = 164;

  memcpy(res, fp_header, 12);
  memcpy(res + 12, req + 144, 20);
  return 0;
}

int fairplay_decrypt(fairplay_t* fp, const unsigned char input[72],
                     unsigned char output[16]) {
  unsigned char cipher[72];

  if (fp->keymsglen != 164) return -1;

  // playfair_decrypt only reads its cipherText, but it declares it non-const,
  // so hand it a copy rather than cast the constness away.
  memcpy(cipher, input, sizeof(cipher));
  playfair_decrypt(fp->keymsg, cipher, output);
  return 0;
}
