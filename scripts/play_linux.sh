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
# FSR 3.1 (anti-aliasing), when the SDK was built with it.
if [ -e "$sdk_lib/libamd_fidelityfx_vk.so" ]; then
  if [ ! -e "$build/libamd_fidelityfx_vk.so" ]; then
    ln -s "$sdk_lib/libamd_fidelityfx_vk.so" "$build/libamd_fidelityfx_vk.so"
  fi
else
  echo "note: the FSR anti-aliasing option wasn't built (the SDK's CMake output says why: it needs the network the first time and Vulkan's development files); the game offers MSAA and off." >&2
fi

# One value per option, the last given: the SDK's command line parser fails on a
# repeated option, and the game then ignores every option on the command line.
option_name() { k="${1%%=*}"; k="${k#--}"; echo "${k#no-}"; }
count=$#
i=0
for arg do
  i=$((i + 1))
  keep=1
  case "$arg" in
    --?*)
      name=$(option_name "$arg")
      j=0
      for later do
        j=$((j + 1))
        if [ "$j" -gt "$i" ] && [ "$j" -le "$count" ]; then
          case "$later" in --?*) [ "$(option_name "$later")" = "$name" ] && keep=0 ;; esac
        fi
      done ;;
  esac
  if [ "$keep" = 1 ]; then set -- "$@" "$arg"; fi
done
shift "$count"

LD_LIBRARY_PATH="$build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$build/mcla" "$@"
