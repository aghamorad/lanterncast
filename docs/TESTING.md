# Validation record

2026-10-11, Samsung QA65Q7FAM / Tizen 3.0 / ARMv7, macOS sender.

- RTSP control and event connections remained open concurrently.
- Mirroring connected and reconnected without restarting the receiver.
- 1,200 consecutive video packets were accepted without decryption errors.
- A received, decrypted H.264 High frame independently decoded to the actual 1920 × 1080 Mac desktop.
- Samsung video and AAC-ELD media players returned successful Play results and advancing playback clocks.
- Ending a sound stream preserved video; full teardown released both RTSP sockets.
- Branding/network update: Samsung network API supplied the real TV identity and LanternCast appeared in macOS discovery; app title/icon loaded in the TV runtime.
- Public signing helper repackaged the unsigned widget successfully with the local TV certificate profile.
- Independent host checks passed for integer boundaries including UINT64_MAX and SHA512 against Python's standard libraries.

A decoded frame and playback clock do not establish a visually verified image on the physical panel. The panel image and audible TV output have not been independently observed. Additional TV models, Windows installation and iPhone sender compatibility are not verified.
