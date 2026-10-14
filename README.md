<p align="center"><img src="res/icon.png" width="160" alt="LanternCast icon"></p>

# LanternCast

An experimental AirPlay screen-mirroring receiver for older Samsung Tizen TVs. Open LanternCast on the TV, then select **LanternCast** in your Mac or iPhone's Screen Mirroring menu.

It runs directly on the TV as a sideloaded widget, using a Native Client module. Video is decoded by the TV's own hardware H.264 decoder and presented with OpenGL ES; audio is decoded inside the module (AAC-ELD) and played through Pepper's audio output, with the audio clock slaved to video. H.264 video and AAC/AAC-ELD audio arrive over the local network. No companion streaming server, subscription or external diagnostic server is required.

**Early release, v0.2.2.** Tested on a Samsung QA65Q7FAM (2017 Q7F), Tizen 3.0, ARMv7. Fullscreen iPhone mirroring is confirmed on the physical panel: the picture filled the screen at the correct proportions, and the report was smooth playback with the sound in sync. Version 0.2.1 read the H.264 source dimensions from the stream, preserved aspect ratio and applied rotation changes. Version 0.2.2 replaces the Samsung media-player pipeline with hardware decode plus OpenGL ES presentation, decodes audio in the module, reorders and conceals lost audio RTP packets, maps the audio clock to video, and handles play, pause, seek and stop. Other TV models remain unverified. This is not a general claim of compatibility with all Samsung TVs.

## Install

Download **LanternCast-0.2.2-unsigned.wgt** from [Releases](https://github.com/aghamorad/lanterncast/releases). The package must be signed with a Samsung certificate profile authorized for your TV before installation. A package signed for somebody else's TV is not a universal installer.

Follow [the installation guide](docs/INSTALL.md) for Developer Mode, certificates, signing and sideloading. The prebuilt release avoids compiling the old NaCl toolchain. Windows users can use Tizen Studio to sign and install; the helper scripts require Bash and Python 3.9+.

After installation, keep the TV and sender on the same local network, open LanternCast, then use **Control Center → Screen Mirroring → LanternCast**. If macOS asks you to choose content, choose the screen and start mirroring. Keep the app open while connected.

## Compatibility and limits

The app needs Samsung's legacy NaCl/Pepper runtime, its hardware video decoder, and the ESDataSource/MediaPlayer interfaces it falls back on when the hardware decoder refuses to start. The release contains an **ARM** executable. TVs without those interfaces, different CPU architectures and non-Tizen TVs are unsupported by this build. Tizen 3.0 is the tested platform; support for newer versions is not established.

Direct URL video casting, protected streaming-service playback, AirPlay 2 pairing/multiroom and standalone ALAC audio are not implemented. More than one sender at a time is not supported. There is no PIN or authentication gate: use it on a trusted local network. It is a prototype, not an Apple-certified AirPlay product or a Samsung store app.

## Build and contribute

See [BUILD.md](docs/BUILD.md). Independent host checks run with `python3 tests/check.py`. The source includes the legacy application ID `MoradAP001.AirPlayTV` to allow an upgrade of the original prototype; the displayed name is LanternCast.

The application reads the TV's IP and MAC through Samsung's network API. No developer LAN addresses, certificates, private signing profiles or session logs are included. Diagnostics remain in the app; outgoing diagnostic collection is disabled by default. An optional local inspector frame capture is disabled until explicitly requested.

Please include TV model/year, firmware/Tizen version, sender OS, and the failing step when reporting an issue. Remove IP/MAC addresses, session bodies and other personal information from logs before sharing them.

## License and credits

GPL-3.0; see [LICENSE.md](LICENSE.md). FairPlay/playfair comes from the UxPlay ecosystem; its upstream license is retained. AES comes from tiny-AES-c and SHA-512 from TweetNaCl, both public domain. Samsung's SDK is an external build dependency and is not redistributed. So is Fraunhofer's FDK-AAC, which the module uses to decode AAC-ELD audio: it is fetched and built locally, it is not committed here, and its license is not compatible with the GPL, so it stands outside this project's GPL-3.0. See [THIRD-PARTY.md](nacl/THIRD-PARTY.md). The original LanternCast icon and application changes are distributed with the project under GPL-3.0.

AirPlay is an Apple trademark. Samsung and Tizen belong to their respective owners. This project is independent of those companies.
