"""Builds the HDR and temporal AA shaders of the fork's SDK patches: copies the XeSL sources
from the SDK tree into the xenia 0b2ffa3 shader tree of the toolchain, compiles
DXBC (fxc cs/ps_5_1 /O3) and SPIR-V (glslang + spirv-opt -O -O
--canonicalize-ids --strip-debug), and writes compact headers (32-bit words, no
disassembly) into the SDK's bytecode directories.

usage: build_shaders.py [name ...]   (default: all listed below)

XENIA_SHADER_TREE: xenia-project/xenia's src/xenia at 0b2ffa3 (gpu/shaders and
ui/shaders are used; the sources are copied in to build). VULKAN_SDK: for
glslangValidator and spirv-opt; FXC_PATH optional (the newest Windows Kits
fxc otherwise)."""
import glob, os, shutil, struct, subprocess, sys, tempfile

SDK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "third_party", "rexglue-sdk")
XENIA = os.environ["XENIA_SHADER_TREE"]
# name: (sdk dir of the source, xenia dir, d3d12 out dir, spirv out dir)
UI = ("src/ui/shaders", "ui/shaders", "src/ui/shaders/bytecode/d3d12_5_1", "src/ui/shaders/vulkan_spirv")
GPU = ("src/graphics/shaders", "gpu/shaders", "src/graphics/shaders/bytecode/d3d12_5_1", "src/graphics/shaders/vulkan_spirv")
SHADERS = {
    "hdr_ui_composite.ps": UI,
    "hdr_refresh.cs": GPU,
    "hdr_fullscreen.vs": GPU,
    "hdr_underlay_restore.ps": GPU,
    "hdr_scene_mcla.cs": GPU,
    "hdr_rt_copy.cs": GPU,
    "hdr_rt_copy_4xmsaa.cs": GPU,
    "taa_motion_vectors_mcla.cs": GPU,
    "taa_luma_snapshot.cs": GPU,
    "taa_reactive_mcla.cs": GPU,
    "taa_debug_view.cs": GPU,
}
# Built for Vulkan only.
VULKAN_ONLY = {"hdr_rt_copy.cs", "hdr_rt_copy_4xmsaa.cs"}
INCLUDES = [("src/ui/shaders/hdr_output.xesli", "ui/shaders"),
            ("src/graphics/shaders/hdr_rt_copy.cs.xesl", "gpu/shaders")]


def words_header(name, words, src_name):
    lines = [f"// Generated from {src_name} with the xenia-project/xenia 0b2ffa3 shader",
             "// sources (see patches/README.md). Not disassembled to keep the patch small.",
             f"const uint32_t {name}[] = {{"]
    for i in range(0, len(words), 8):
        lines.append("    " + " ".join(f"0x{w:08X}," for w in words[i:i + 8]))
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    names = sys.argv[1:] or list(SHADERS)
    for inc, xdir in INCLUDES:
        if os.path.exists(os.path.join(SDK, inc)):
            shutil.copy(os.path.join(SDK, inc), os.path.join(XENIA, xdir))
    vk = os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\1.4.363.0")
    for n in names:
        sdir, xdir, ddir, vdir = SHADERS[n]
        src_name = n + ".xesl"
        shutil.copy(os.path.join(SDK, sdir, src_name), os.path.join(XENIA, xdir))
        src = os.path.join(XENIA, xdir, src_name)
        ident = n.replace(".", "_")
        stage = n[-2:]
        tmp = tempfile.mkdtemp()
        dxbc_size = 0
        if n not in VULKAN_ONLY:
            fxc = os.environ.get("FXC_PATH") or sorted(glob.glob(
                r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe"))[-1]
            dxbc = os.path.join(tmp, "s.dxbc")
            r = subprocess.run([fxc, "/D", "SHADING_LANGUAGE_HLSL_XE=1", "/I", os.path.dirname(src),
                                "/Fo", dxbc, "/T", f"{stage}_5_1", "/O3", "/Qstrip_reflect",
                                "/Qstrip_debug", "/Qstrip_priv", "/Gfp", "/nologo", src],
                               capture_output=True, text=True)
            if r.returncode:
                print(r.stdout, r.stderr)
                sys.exit(f"fxc failed: {n}")
            data = open(dxbc, "rb").read()
            dxbc_size = len(data)
            data += bytes(-len(data) % 4)
            words = struct.unpack(f"<{len(data) // 4}I", data)
            open(os.path.join(SDK, ddir, ident + ".h"), "w", newline="\n").write(
                words_header(ident, words, src_name))
        stages = {"vs": "vert", "ps": "frag", "cs": "comp"}
        wrapper = ("#version 460\n#extension GL_EXT_control_flow_attributes : require\n"
                   "#extension GL_EXT_samplerless_texture_functions : require\n"
                   "#extension GL_GOOGLE_include_directive : require\n"
                   f"#include \"{src_name}\"\n")
        spv0, spv = os.path.join(tmp, "a.spv"), os.path.join(tmp, "b.spv")
        r = subprocess.run([os.path.join(vk, "bin", "glslangValidator"), "--stdin",
                            "-DSHADING_LANGUAGE_GLSL_XE=1", "-S", stages[stage], "-o", spv0, "-V",
                            f"-I{os.path.dirname(src)}"], input=wrapper, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout, r.stderr)
            sys.exit(f"glslang failed: {n}")
        r = subprocess.run([os.path.join(vk, "bin", "spirv-opt"), "-O", "-O", "--canonicalize-ids",
                            "--strip-debug", spv0, "-o", spv], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout, r.stderr)
            sys.exit(f"spirv-opt failed: {n}")
        data = open(spv, "rb").read()
        words = struct.unpack(f"<{len(data) // 4}I", data)
        open(os.path.join(SDK, vdir, ident + ".h"), "w", newline="\n").write(
            words_header(ident, words, src_name))
        print(f"{n}: dxbc {dxbc_size} B, spirv {len(data)} B")
        shutil.rmtree(tmp)


if __name__ == "__main__":
    main()
