// The legacy AirPlay FairPlay handshake, as served at POST /fp-setup.
//
// Three calls, and only the third one is cryptography:
//
//   fairplay_setup()      16-byte request  -> 142-byte reply   (table lookup)
//   fairplay_handshake()  164-byte request -> 32-byte reply    (header + echo)
//   fairplay_decrypt()    72-byte ekey     -> 16-byte AES key  (playfair)
//
// The shapes match UxPlay's lib/fairplay.h, so the two can be read side by
// side.  What differs is that this carries its state in the struct the caller
// owns instead of a heap allocation, because a NaCl instance has no allocator
// we want to lean on.
#ifndef AIRPLAY_FAIRPLAY_H_
#define AIRPLAY_FAIRPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fairplay_s {
  // Stage 2's request, kept because stage 3 needs it as the playfair message.
  unsigned char keymsg[164];
  unsigned int keymsglen;
} fairplay_t;

// Answer stage 1.  Byte 4 must be 0x03 (the version this table serves) and
// byte 14 selects which of the four canned blobs to send back.  No crypto.
// Returns 0 on success.
int fairplay_setup(fairplay_t* fp, const unsigned char req[16],
                   unsigned char res[142]);

// Answer stage 2, and remember the request: the last 20 of its 164 bytes are
// echoed straight back, and the whole thing becomes playfair's message.
// Returns 0 on success.
int fairplay_handshake(fairplay_t* fp, const unsigned char req[164],
                       unsigned char res[32]);

// Turn the 72-byte ekey out of SETUP into the 16-byte AES key that decrypts
// the media stream.  Requires a preceding fairplay_handshake().  Returns 0 on
// success.
int fairplay_decrypt(fairplay_t* fp, const unsigned char input[72],
                     unsigned char output[16]);

#ifdef __cplusplus
}
#endif

#endif  // AIRPLAY_FAIRPLAY_H_
