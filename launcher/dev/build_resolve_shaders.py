"""Builds the direct resolve shaders (src/graphics/shaders/resolve_direct*.xesl) of the fork's SDK patches, for both APIs.

Copies the XeSL sources from the SDK tree into the xenia-project/xenia 0b2ffa3 shader tree (src/xenia/gpu/shaders and
src/xenia/ui/shaders of that commit), compiles DXBC (fxc cs_5_1 /O3) and SPIR-V (glslangValidator, then spirv-opt -O -O
--canonicalize-ids --strip-debug) and writes headers of 32-bit words, no disassembly, into the SDK's bytecode folders
(the format of rexglue-texture-load-direct.patch and rexglue-direct-resolve.patch).

  python -I build_resolve_shaders.py [--sdk DIR] [--xenia DIR] [--check] [--only text] [--skip text] [name ...]

  name        a shader without its extension, as in the headers' names: resolve_direct_fast_32bpp_msaa_scaled_cs
              (default: every resolve_direct_*.cs.xesl in the SDK)
  --check     compile and compare with the header on disk, write nothing (the toolchain rebuilds the SDK's shaders
              from their sources bit for bit when this says "same")
  --xenia     the xenia 0b2ffa3 tree with gpu/shaders and ui/shaders (default: environment XENIA_SHADER_TREE)
  --sdk       the ReXGlue SDK (default: ../../third_party/rexglue-sdk from this file)
Needs fxc (the newest Windows Kits, or FXC_PATH) and the Vulkan SDK (VULKAN_SDK, default the newest in C:\\VulkanSDK).
"""
import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GPU_SRC = "src/graphics/shaders"
D3D_OUT = "src/graphics/shaders/bytecode/d3d12_5_1"
VK_OUT = "src/graphics/shaders/vulkan_spirv"


def words_header(name, words, src_name):
    lines = [f"// Generated from {src_name} with the xenia-project/xenia 0b2ffa3 shader",
             "// sources (see resolve_direct.xesli). Not disassembled to keep the patch small.",
             f"const uint32_t {name}[] = {{"]
    for i in range(0, len(words), 8):
        lines.append("    " + " ".join(f"0x{w:08X}," for w in words[i:i + 8]))
    lines.append("};")
    return "\n".join(lines) + "\n"


def read_words(path):
    text = open(path, encoding="utf-8").read()
    body = text[text.index("{") + 1:text.rindex("}")]
    return [int(t.strip(), 16) for t in body.replace("\n", " ").split(",") if t.strip()]


def vulkan_sdk():
    v = os.environ.get("VULKAN_SDK")
    if v:
        return v
    cand = sorted(glob.glob(r"C:\VulkanSDK\*"))
    if not cand:
        sys.exit("no Vulkan SDK (VULKAN_SDK)")
    return cand[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--sdk", default=os.path.normpath(os.path.join(HERE, "..", "..", "third_party", "rexglue-sdk")))
    ap.add_argument("--xenia", default=os.environ.get("XENIA_SHADER_TREE"))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="", help="leave out the shaders whose name has this text")
    ap.add_argument("--api", choices=["both", "d3d12", "vulkan"], default="both")
    a = ap.parse_args()
    if not a.xenia:
        sys.exit("--xenia or XENIA_SHADER_TREE")
    sdk, xenia = a.sdk, a.xenia
    srcdir = os.path.join(sdk, GPU_SRC)
    names = a.names or sorted(os.path.basename(p)[:-len(".xesl")].replace(".", "_")
                              for p in glob.glob(os.path.join(srcdir, "resolve_direct_*.cs.xesl")))
    if a.only:
        names = [n for n in names if a.only in n]
    if a.skip:
        names = [n for n in names if a.skip not in n]
    # Includes first: the .xesli files of the SDK replace the tree's.
    xg = os.path.join(xenia, "gpu", "shaders")
    for inc in glob.glob(os.path.join(srcdir, "resolve_direct*.xesli")):
        shutil.copy(inc, xg)
    fxc = os.environ.get("FXC_PATH") or sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe"))[-1]
    vk = vulkan_sdk()
    status = 0
    for ident in names:
        src_name = ident[:-3] + ".cs.xesl"
        if not os.path.exists(os.path.join(srcdir, src_name)):
            sys.exit(f"no source {src_name}")
        shutil.copy(os.path.join(srcdir, src_name), xg)
        src = os.path.join(xg, src_name)
        tmp = tempfile.mkdtemp()
        sizes = []
        try:
            if a.api in ("both", "d3d12"):
                dxbc = os.path.join(tmp, "s.dxbc")
                r = subprocess.run([fxc, "/D", "SHADING_LANGUAGE_HLSL_XE=1", "/I", xg, "/Fo", dxbc, "/T", "cs_5_1", "/O3",
                                    "/Qstrip_reflect", "/Qstrip_debug", "/Qstrip_priv", "/Gfp", "/nologo", src],
                                   capture_output=True, text=True)
                if r.returncode:
                    print(r.stdout, r.stderr)
                    sys.exit(f"fxc failed: {ident}")
                data = open(dxbc, "rb").read()
                n = len(data)
                data += bytes(-len(data) % 4)
                words = list(struct.unpack(f"<{len(data) // 4}I", data))
                path = os.path.join(sdk, D3D_OUT, ident + ".h")
                if a.check:
                    old = read_words(path) if os.path.exists(path) else None
                    same = old == words
                    sizes.append(f"d3d12 {'same' if same else 'DIFFERENT'} ({n} B new, {len(old) * 4 if old else '-'} B old)")
                    status |= 0 if same else 1
                else:
                    open(path, "w", newline="\n").write(words_header(ident, words, src_name))
                    sizes.append(f"dxbc {n} B")
            if a.api in ("both", "vulkan"):
                wrapper = ("#version 460\n#extension GL_EXT_control_flow_attributes : require\n"
                           "#extension GL_EXT_samplerless_texture_functions : require\n"
                           "#extension GL_GOOGLE_include_directive : require\n"
                           f"#include \"{src_name}\"\n")
                spv0, spv = os.path.join(tmp, "a.spv"), os.path.join(tmp, "b.spv")
                r = subprocess.run([os.path.join(vk, "bin", "glslangValidator"), "--stdin", "-DSHADING_LANGUAGE_GLSL_XE=1",
                                    "-S", "comp", "-o", spv0, "-V", f"-I{xg}"], input=wrapper, capture_output=True, text=True)
                if r.returncode:
                    print(r.stdout, r.stderr)
                    sys.exit(f"glslang failed: {ident}")
                r = subprocess.run([os.path.join(vk, "bin", "spirv-opt"), "-O", "-O", "--canonicalize-ids", "--strip-debug",
                                    spv0, "-o", spv], capture_output=True, text=True)
                if r.returncode:
                    print(r.stdout, r.stderr)
                    sys.exit(f"spirv-opt failed: {ident}")
                data = open(spv, "rb").read()
                words = list(struct.unpack(f"<{len(data) // 4}I", data))
                path = os.path.join(sdk, VK_OUT, ident + ".h")
                if a.check:
                    old = read_words(path) if os.path.exists(path) else None
                    same = old == words
                    sizes.append(f"spirv {'same' if same else 'DIFFERENT'} ({len(data)} B new, {len(old) * 4 if old else '-'} B old)")
                    status |= 0 if same else 1
                else:
                    open(path, "w", newline="\n").write(words_header(ident, words, src_name))
                    sizes.append(f"spirv {len(data)} B")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        print(f"{ident}: " + ", ".join(sizes), flush=True)
    sys.exit(status)


if __name__ == "__main__":
    main()
