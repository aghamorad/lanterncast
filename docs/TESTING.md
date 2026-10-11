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

An iPhone photograph supplied by the user subsequently confirmed actual video on the physical panel, stretched to the TV aspect ratio. Version 0.2.1 requests Samsung LETTERBOX mode, fits a centered display rectangle when older firmware rejects that mode, reads cropped dimensions from H.264 SPS when AirPlay headers contain zero dimensions, and applies subsequent configuration changes for rotation. Independent FFmpeg/libx264 fixtures cover portrait 720 × 1280, landscape 1280 × 720, cropped portrait 1170 × 2532, baseline 1920 × 1080, and truncated inputs. These checks passed. The updated ARM build was installed on the same TV. Its legacy firmware rejected SetDisplayMode with -12; SetDisplayRect accepted the fitted rectangle with result 0, and Mac mirroring resumed with Play=0 and an advancing playback clock. Corrected iPhone proportions and rotation on the physical panel still require confirmation. Audible output, additional TV models and Windows installation remain unverified.
