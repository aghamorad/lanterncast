# Build from source

The release includes a prebuilt ARM module in the unsigned widget. Building it yourself requires Samsung's **Pepper 47 NaCl SDK**, a supported host toolchain, Python 3, Make and Tizen Studio for signing. The SDK is not bundled. See Samsung's [NaCl setup](https://developer.samsung.com/smarttv/develop/extension-libraries/nacl/getting-started.html) and [toolchain documentation](https://developer.samsung.com/smarttv/develop/extension-libraries/nacl/using-tizen-studio-for-nacl/managing-nacl-toolchains.html).

The verified build host is macOS. Linux toolchain selection is supported by the Makefile's host option but has not been tested. The old SDK's Python launchers may require a Python 3 compatibility port on modern systems; an unmodified SDK is not promised to work. Do not replace the SDK with Chromium's generic Pepper SDK: this source uses Samsung extensions.

```sh
export PEPPER_DIR=/path/to/pepper_47
./scripts/build.sh
python3 tests/check.py
./scripts/sign.sh dist/LanternCast-0.2.0-unsigned.wgt YourSamsungProfile
./scripts/install.sh TV_IP dist/LanternCast-0.2.0-signed.wgt
```

`PEPPER_DIR` must contain `include`, `lib/pnacl/Release` and `toolchain/mac_pnacl/bin`. For Linux set `PNACL_HOST=linux` when invoking Make or the build script. Host checks require `clang` and `clang++`; they test binary-plist integer encoding independently with Python's plistlib and SHA512 against hashlib.

The native binary is finalized PNaCl translated to ARM. The widget contains only the manifest, HTML shell, icon, module manifest and ARM executable. `scripts/package.py` refuses to package a binary older than its C/C++ sources. Signing occurs in a temporary folder inside `.build`, removed on completion, and the signed widget goes to `dist`.

The application uses the TV's network API for discovery identity. `REPORT_URL` in `index.html` is empty by default; set it to your own local endpoint only for a diagnostic build. Full protocol logs can contain identifying session data. Never publish private certificate profiles or logs.

The local inspector can request `document.getElementById('nacl').postMessage('capture-next-frame')`; the next received frame appears as `window.diagnosticFrame`. Capturing a frame can expose desktop content and is disabled by default. It is a diagnostic aid, not a physical-TV screenshot.
