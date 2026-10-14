# Build from source

The release includes a prebuilt ARM module in the unsigned widget. Building it yourself requires Samsung's **Pepper 47 NaCl SDK**, a supported host toolchain, Python 3, Make and Tizen Studio for signing. The SDK is not bundled. See Samsung's [NaCl setup](https://developer.samsung.com/smarttv/develop/extension-libraries/nacl/getting-started.html) and [toolchain documentation](https://developer.samsung.com/smarttv/develop/extension-libraries/nacl/using-tizen-studio-for-nacl/managing-nacl-toolchains.html).

The verified build host is macOS. Linux toolchain selection is supported by the Makefile's host option but has not been tested. The old SDK's Python launchers may require a Python 3 compatibility port on modern systems; an unmodified SDK is not promised to work. Do not replace the SDK with Chromium's generic Pepper SDK: this source uses Samsung extensions.

```sh
export PEPPER_DIR=/path/to/pepper_47
./scripts/fetch-fdk-aac.sh          # once: builds the AAC decoder into .build/
./scripts/build.sh
python3 tests/check.py
./scripts/sign.sh dist/LanternCast-0.2.2-unsigned.wgt YourSamsungProfile
./scripts/install.sh TV_IP dist/LanternCast-0.2.2-signed.wgt
```

`PEPPER_DIR` must contain `include`, `lib/pnacl/Release` and `toolchain/mac_pnacl/bin`. For Linux set `PNACL_HOST=linux` when invoking Make or the build script. Host checks require `clang` and `clang++`; they test binary-plist integer encoding independently with Python's plistlib and SHA512 against hashlib.

**The audio decoder is not in this repository.** AirPlay sends AAC-ELD and the module decodes it itself, which needs Fraunhofer's FDK-AAC. It is an external dependency with its own license and is not GPL-compatible; see [THIRD-PARTY.md](../nacl/THIRD-PARTY.md). `scripts/fetch-fdk-aac.sh` clones FDK 2.0.3 and builds it as a static PNaCl library into `.build/` (gitignored), so nothing FDK-related is installed system-wide or committed. The build needs CMake 3.5+ and a few minutes. If you already have FDK 2.0.3 built for the same toolchain, set `FDK_DIR` and `FDK_LIB` instead and skip the script.

The SDK's `pnacl-clang` wrapper shells out to `python`, which modern macOS lacks. Both the Makefile and the fetch script set `PNACLPYTHON=python3` for that reason; if you drive the SDK by hand, export it yourself. You also need `python3` on `PATH` for the Makefile's per-target include paths.

The native binary is finalized PNaCl translated to ARM. The widget contains only the manifest, HTML shell, icon, module manifest and ARM executable. `scripts/package.py` refuses to package a binary older than its C/C++ sources. Signing occurs in a temporary folder inside `.build`, removed on completion, and the signed widget goes to `dist`.

The application uses the TV's network API for discovery identity. `REPORT_URL` in `index.html` is empty by default; set it to your own local endpoint only for a diagnostic build. Full protocol logs can contain identifying session data. Never publish private certificate profiles or logs.

The local inspector can request `document.getElementById('nacl').postMessage('capture-next-frame')`; the next received frame appears as `window.diagnosticFrame`. Capturing a frame can expose desktop content and is disabled by default. It is a diagnostic aid, not a physical-TV screenshot.
