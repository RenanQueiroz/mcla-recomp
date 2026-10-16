#!/bin/sh
# Starts the Linux build. The SDK loads the Xenos GPU plugin from the
# executable's folder only, so the SDK libraries are linked in next to mcla
# on first run. Options are passed on to mcla.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/out/build/linux-amd64-release"
sdk_lib="$root/third_party/rexglue-sdk/out/install/linux-amd64/lib"

if [ ! -x "$build/mcla" ]; then
  echo "error: $build/mcla not found; build it first (README.md, 'Linux')" >&2
  exit 1
fi
for lib in librexruntime.so librexgpu-xenos.so; do
  if [ ! -e "$build/$lib" ]; then
    ln -s "$sdk_lib/$lib" "$build/$lib"
  fi
done

LD_LIBRARY_PATH="$build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$build/mcla" "$@"
