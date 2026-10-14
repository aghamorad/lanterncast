#!/usr/bin/env bash
# Fetch and build FDK-AAC 2.0.3 as a static library for the Pepper 47 PNaCl
# toolchain, into .build/.  The Makefile links the result for the AAC-ELD audio
# decoder; see nacl/THIRD-PARTY.md for why it is not vendored.
#
# Everything lands in .build/, which is gitignored, so nothing here is committed
# and nothing is installed system-wide.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${PEPPER_DIR:?Set PEPPER_DIR to the Samsung Pepper 47 SDK directory}"

PNACL_HOST="${PNACL_HOST:-mac}"
PNACL_BIN="$PEPPER_DIR/toolchain/${PNACL_HOST}_pnacl/bin"
[ -x "$PNACL_BIN/pnacl-clang" ] || {
  echo "No pnacl-clang under $PNACL_BIN." >&2
  echo "Check PEPPER_DIR, or set PNACL_HOST if this is not a macOS host." >&2
  exit 1
}

# The SDK's driver wrappers call `python` unless told otherwise, and this host
# has only python3.  The Makefile sets the same variable for the module build;
# CMake's configure probes need it in the environment too, or the compiler
# check fails with "python: command not found".
export PNACLPYTHON="${PNACLPYTHON:-python3}"

FDK_TAG="v2.0.3"
FDK_URL="https://github.com/mstorsjo/fdk-aac.git"
BUILD="$ROOT/.build"
SRC="$BUILD/fdk-aac"
OUT="$BUILD/fdk-nacl"

mkdir -p "$BUILD"

# 1. Source.  A shallow clone of the release tag is enough; FDK carries its own
# NOTICE and license terms, which the clone brings with it.
if [ -d "$SRC/.git" ]; then
  echo "== FDK source already present at $SRC =="
  HAVE="$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || true)"
  [ "$HAVE" = "$FDK_TAG" ] || echo "   note: checkout is '$HAVE', expected $FDK_TAG"
else
  echo "== fetching FDK-AAC $FDK_TAG =="
  git clone --depth 1 --branch "$FDK_TAG" "$FDK_URL" "$SRC"
fi

# 2. Toolchain file.  Generated rather than committed: it has to point at the
# SDK on *this* machine.  CMAKE_TRY_COMPILE_TARGET_TYPE is essential -- the
# PNaCl driver cannot run a linked test binary, so CMake must only compile a
# static library when probing the compiler.
TOOLCHAIN="$BUILD/fdk-nacl-toolchain.cmake"
cat > "$TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR pnacl)
set(CMAKE_C_COMPILER   "$PNACL_BIN/pnacl-clang")
set(CMAKE_CXX_COMPILER "$PNACL_BIN/pnacl-clang++")
set(CMAKE_AR           "$PNACL_BIN/pnacl-ar")
set(CMAKE_RANLIB       "$PNACL_BIN/pnacl-ranlib")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_CXX_FLAGS "-O2 -std=gnu++11")
EOF

# 3. Configure and build.  The build itself takes a few minutes.
# An interrupted configure caches its failed compiler probe, so the build
# directory starts clean on every run rather than trusting a stale cache.
[ -d "$OUT" ] && rm -rf -- "$OUT"

echo "== configuring =="
cmake -S "$SRC" -B "$OUT" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF

echo "== building =="
cmake --build "$OUT" --parallel

[ -f "$OUT/libfdk-aac.a" ] || { echo "FAIL: $OUT/libfdk-aac.a was not produced" >&2; exit 1; }
echo "OK: $OUT/libfdk-aac.a"
echo "nacl/Makefile picks this up by default; no further setup needed."
