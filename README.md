<p align="center"><img src="res/icon.png" width="160" alt="LanternCast icon"></p>

# LanternCast

An experimental AirPlay screen-mirroring receiver for older Samsung Tizen TVs. Open LanternCast on the TV, then select **LanternCast** in your Mac's Screen Mirroring menu.

It runs directly on the TV as a sideloaded widget, using a Native Client module and Samsung's native media player. It receives H.264 video and AAC/AAC-ELD audio over the local network. No companion streaming server, subscription or external diagnostic server is required.

**Early release, v0.2.0.** Tested on a Samsung QA65Q7FAM (2017 Q7F), Tizen 3.0, ARMv7. The Mac connected, reconnected and sent valid 1080p desktop frames; the TV's video/audio players started and their playback clocks advanced. The physical TV image and audible output were not independently observed. Other TV models and iPhone mirroring remain unverified. This is not a general claim of compatibility with all Samsung TVs.

## Install

Download **LanternCast-0.2.0-unsigned.wgt** from [Releases](https://github.com/aghamorad/lanterncast/releases). The package must be signed with a Samsung certificate profile authorized for your TV before installation. A package signed for somebody else's TV is not a universal installer.

Follow [the installation guide](docs/INSTALL.md) for Developer Mode, certificates, signing and sideloading. The prebuilt release avoids compiling the old NaCl toolchain. Windows users can use Tizen Studio to sign and install; the helper scripts require Bash and Python 3.9+.

After installation, keep the TV and Mac on the same local network, open LanternCast, then use **Control Center → Screen Mirroring → LanternCast**. If macOS asks you to choose content, choose the screen and start mirroring. Keep the app open while connected.

## Compatibility and limits

The app needs Samsung's legacy NaCl/Pepper runtime and Samsung ESDataSource/MediaPlayer interfaces. The release contains an **ARM** executable. TVs without those interfaces, different CPU architectures and non-Tizen TVs are unsupported by this build. Tizen 3.0 is the tested platform; support for newer versions is not established.

Direct URL video casting, protected streaming-service playback, AirPlay 2 pairing/multiroom and standalone ALAC audio are not implemented. Audio/video synchronization, RTP loss recovery and simultaneous senders remain incomplete. There is no PIN or authentication gate: use it on a trusted local network. It is a prototype, not an Apple-certified AirPlay product or a Samsung store app.

## Build and contribute

See [BUILD.md](docs/BUILD.md). Independent host checks run with `python3 tests/check.py`. The source includes the legacy application ID `MoradAP001.AirPlayTV` to allow an upgrade of the original prototype; the displayed name is LanternCast.

The application reads the TV's IP and MAC through Samsung's network API. No developer LAN addresses, certificates, private signing profiles or session logs are included. Diagnostics remain in the app; outgoing diagnostic collection is disabled by default. An optional local inspector frame capture is disabled until explicitly requested.

Please include TV model/year, firmware/Tizen version, sender OS, and the failing step when reporting an issue. Remove IP/MAC addresses, session bodies and other personal information from logs before sharing them.

## License and credits

GPL-3.0; see [LICENSE.md](LICENSE.md). FairPlay/playfair comes from the UxPlay ecosystem; its upstream license is retained. AES comes from tiny-AES-c and SHA-512 from TweetNaCl, both public domain. See [THIRD-PARTY.md](nacl/THIRD-PARTY.md). Samsung's SDK is an external build dependency and is not redistributed. The original LanternCast icon and application changes are distributed with the project under GPL-3.0.

AirPlay is an Apple trademark. Samsung and Tizen belong to their respective owners. This project is independent of those companies.
