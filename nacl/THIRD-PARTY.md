AES: kokke/tiny-AES-c, public domain, source retrieved 2026-10-11.
SHA-512: TweetNaCl 20140427 crypto_hash and dependencies extracted verbatim, public domain. https://tweetnacl.cr.yp.to/20140427/tweetnacl.c
FairPlay: vendored UxPlay playfair sources, retain upstream licensing.

FDK-AAC: NOT vendored and NOT covered by this project's GPL-3.0 license.

  `nacl/audio_sink.h` decodes the AAC-ELD stream itself and feeds PCM to Pepper's
  audio output, which requires the Fraunhofer FDK AAC Codec Library (2.0.3, via
  https://github.com/mstorsjo/fdk-aac at tag v2.0.3). It is an external build
  dependency fetched at build time by `scripts/fetch-fdk-aac.sh` into `.build/`,
  which is gitignored: no FDK source or binary is distributed in this repository.
  FDK ships its own `NOTICE`, its own license terms and a patent-licensing
  statement; read those before building or redistributing a binary that links it.
  Its terms are not compatible with the GPL, so the FDK code and any binary
  linking it are separate from, and not licensed under, this project's GPL-3.0.
