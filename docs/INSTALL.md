# Install LanternCast on a TV

## 1. Check the TV

The tested device is the 2017 Samsung Q7F QA65Q7FAM, Tizen 3.0, ARMv7. The app needs the legacy NaCl runtime. A modern TV's ability to install a widget does not prove it can run this module. Other models are experimental.

Use a computer and TV on the same home network. Find the TV's IPv4 address in its network settings and the computer's LAN IPv4 address in its network settings. Avoid guest networks and client isolation.

## 2. Set up Samsung tools

Install [Tizen Studio and the Samsung TV Extension](https://developer.samsung.com/smarttv/develop/getting-started/setting-up-sdk/installing-tv-sdk.html). Install the Samsung Certificate Extension through Package Manager. A Samsung account is needed for a Samsung TV certificate profile.

On the TV, open **Apps**, enter **1 2 3 4 5** using the remote's number entry, enable **Developer Mode**, enter the computer's LAN address and restart the TV. Menu details can vary with firmware. Samsung's [TV device guide](https://developer.samsung.com/smarttv/develop/getting-started/using-sdk/tv-device.html) describes the connection procedure.

In Tizen Studio Device Manager, add the TV at its IP address, port **26101**, and connect. Alternatively:

```sh
sdb connect TV_IP:26101
sdb devices
```

Replace `TV_IP` with your TV's address. The device should appear connected.

## 3. Create a profile for your TV

Open **Certificate Manager**, create a **Samsung TV** profile, and include your TV's DUID in its distributor certificate. Keep the author certificate, password and distributor certificate private. In Device Manager, use **Permit to install applications** if that option is offered. Follow Samsung's [certificate instructions](https://developer.samsung.com/smarttv/develop/getting-started/setting-up-sdk/creating-certificates.html).

The manifest requests `internet`, `network.public` and `drmplay`. Your certificate must authorize the requested privileges. The tested build used a partner-level profile. Installation with a public-level profile has not been established; a privilege rejection may require a different authorized profile and cannot be fixed by simply renaming the package.

## 4. Sign the release

Download the unsigned `.wgt` from [Releases](https://github.com/aghamorad/lanterncast/releases), and clone or download this repository for the helper scripts. With Tizen's CLI on PATH:

```sh
./scripts/sign.sh /path/to/LanternCast-0.2.2-unsigned.wgt YourSamsungProfile
```

The result is `dist/LanternCast-0.2.2-signed.wgt`. If Tizen is not on PATH, set `TIZEN` to the full path of its CLI executable. The script uses your existing profile; it does not create or upload certificates.

On Windows, extract the unsigned widget into a folder, import it as a Tizen web project, select your Samsung profile and package/sign it using Tizen Studio. This GUI workflow is based on Samsung's tooling and has not been tested with this release. The Bash helper's legacy `tv-samsung-7.0` packaging metadata matches the tooling used for the tested Tizen 3.0 TV; it is not the app's required runtime version.

## 5. Install and open

On macOS/Linux with `sdb` on PATH:

```sh
./scripts/install.sh TV_IP dist/LanternCast-0.2.2-signed.wgt
```

The helper uses the TV installer command verified on the Q7F. Check its output for a successful install. Other firmware may need Tizen Studio's **Run As → Tizen Web Application** deployment workflow instead. The script does not automatically launch the TV app.

Open **LanternCast** from the TV's Apps panel. On the Mac select **Control Center → Screen Mirroring → LanternCast**. Choose the screen and start mirroring if macOS asks. The TV app must remain open. Stop mirroring from the Mac menu when finished.

## Troubleshooting

- **TV absent from Device Manager:** recheck Developer Mode, the host computer address, restart and port 26101. Disable network client isolation.
- **Certificate, privilege or installation error:** check the Samsung profile, DUID and authorization for all manifest privileges. An unsigned widget cannot be installed directly.
- **LanternCast absent from the Mac menu:** open the app, wait for startup, confirm both devices share a LAN and that multicast DNS is allowed. The app reads its IP automatically.
- **Native module fails to load:** your model may lack the required NaCl runtime, ARM architecture or Samsung media interfaces.
- **Connected but no picture/sound:** report model, firmware and sender OS; this prototype has limited compatibility and synchronization.
- **Debug launch stays on Starting:** the tested TV's debug runtime sometimes needs an inspector attachment and page reload. Opening the app normally from Apps avoids this debug-specific route.

Uninstall LanternCast using the TV's Apps management menu. Developer Mode is needed for sideloading; this is not a USB installation package or a Samsung store listing.
