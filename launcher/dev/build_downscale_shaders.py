r"""Builds the SDK's resolve_downscale compute shader (src/graphics/shaders/resolve_downscale.cs.hlsl and .cs.glsl) into its two headers:

  src/graphics/shaders/bytecode/d3d12_5_1/resolve_downscale_cs.h     fxc /T cs_5_1 /O3 /all_resources_bound /Qstrip_reflect /Qstrip_debug /Qstrip_priv /Fh
  src/graphics/shaders/vulkan_spirv/resolve_downscale_cs.h           glslangValidator -V --target-env vulkan1.0, spirv-opt -O -O --canonicalize-ids
                                                                     --strip-debug, spirv-dis (the layout of the SDK's other headers of these shaders)

  python -I build_downscale_shaders.py [--sdk DIR] [--check]

  --check   build in memory and compare with the headers on disk, write nothing; exit 1 if they differ. Run on the unchanged SDK it says that this toolchain
            (the newest Windows Kits' fxc and the newest Vulkan SDK) rebuilds the SDK's own headers of this shader byte for byte from the unchanged sources (2026-10-28,
            fxc 10.0.26100.0 and Vulkan SDK 1.4.363.0: it did), so a changed header is the source change and nothing else.
  --sdk     the ReXGlue SDK tree (default: ../../third_party/rexglue-sdk from this file)
Needs fxc (the newest Windows Kits, or FXC_PATH) and the Vulkan SDK (VULKAN_SDK, default the newest in C:\VulkanSDK).
"""
import argparse
import glob
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SHADERS = os.path.join("src", "graphics", "shaders")
D3D_HEADER = os.path.join(SHADERS, "bytecode", "d3d12_5_1", "resolve_downscale_cs.h")
VK_HEADER = os.path.join(SHADERS, "vulkan_spirv", "resolve_downscale_cs.h")
NAME = "resolve_downscale_cs"


def run(cmd, **kwargs):
    r = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if r.returncode:
        sys.exit(f"{os.path.basename(cmd[0])} failed:\n{r.stdout}\n{r.stderr}")
    return r


def d3d12_header(sdk, tmp):
    fxc = os.environ.get("FXC_PATH") or sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe"))[-1]
    out = os.path.join(tmp, "fxc.h")
    run([fxc, "/nologo", "/T", "cs_5_1", "/E", "main", "/O3", "/all_resources_bound", "/Qstrip_reflect", "/Qstrip_debug", "/Qstrip_priv",
         "/Fh", out, "/Vn", NAME, os.path.join(sdk, SHADERS, "resolve_downscale.cs.hlsl")])
    text = open(out, encoding="utf-8").read().replace("\r\n", "\n")
    a = text.index("const BYTE ")
    comment = "\n".join(line.rstrip() for line in text[:a].rstrip("\n").split("\n"))  # the SDK's headers have no trailing blanks
    body = text[text.index("{", a) + 1:text.rindex("}")]
    values = [int(v) for v in body.replace("\n", " ").split(",") if v.strip()]
    # 20 a row, each column as wide as its widest value (clang-format's layout of the SDK's headers)
    rows = [values[i:i + 20] for i in range(0, len(values), 20)]
    widths = [max(len(str(row[j])) for row in rows if j < len(row)) for j in range(20)]
    lines = []
    for n, row in enumerate(rows):
        cells = []
        for j, v in enumerate(row):
            last = n == len(rows) - 1 and j == len(row) - 1
            cell = str(v) + ("" if last else ",")
            cells.append(cell if j == len(row) - 1 else cell.ljust(widths[j] + 1))
        lines.append("    " + " ".join(cells))
    return comment + "\n\nconst BYTE " + NAME + "[] = {\n" + "\n".join(lines) + "};\n"


def vulkan_header(sdk, tmp):
    vk = os.environ.get("VULKAN_SDK") or sorted(glob.glob(r"C:\VulkanSDK\*"))[-1]
    spv0 = os.path.join(tmp, "a.spv")
    spv = os.path.join(tmp, "b.spv")
    run([os.path.join(vk, "bin", "glslangValidator"), "-V", "-S", "comp", "--target-env", "vulkan1.0", "-o", spv0,
         os.path.join(sdk, SHADERS, "resolve_downscale.cs.glsl")])
    run([os.path.join(vk, "bin", "spirv-opt"), "-O", "-O", "--canonicalize-ids", "--strip-debug", spv0, "-o", spv])
    dis = run([os.path.join(vk, "bin", "spirv-dis"), spv]).stdout.replace("\r\n", "\n").rstrip("\n")
    data = open(spv, "rb").read()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    rows = ["    " + " ".join(f"0x{w:08X}," for w in words[i:i + 8]) for i in range(0, len(words), 8)]
    return "// Generated with `xb buildshaders`.\n#if 0\n" + dis + "\n#endif\n\nconst uint32_t " + NAME + "[] = {\n" + "\n".join(rows) + "\n};\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", default=os.path.normpath(os.path.join(HERE, "..", "..", "third_party", "rexglue-sdk")))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    status = 0
    with tempfile.TemporaryDirectory() as tmp:
        for label, rel, text in (("D3D12", D3D_HEADER, d3d12_header(a.sdk, tmp)), ("Vulkan", VK_HEADER, vulkan_header(a.sdk, tmp))):
            path = os.path.join(a.sdk, rel)
            if a.check:
                old = open(path, encoding="utf-8", newline="").read().replace("\r\n", "\n") if os.path.exists(path) else None
                same = old == text
                print(f"{label}: {'same as' if same else 'DIFFERENT from'} {rel} ({len(text)} bytes built, {len(old) if old else '-'} on disk)")
                status |= 0 if same else 1
            else:
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
                print(f"{label}: wrote {rel} ({len(text)} bytes)")
    sys.exit(status)


if __name__ == "__main__":
    main()
