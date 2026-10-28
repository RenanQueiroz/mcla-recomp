r"""Checks, with no GPU and no game, which host pixel the SDK's resolve_downscale pass copies for each guest pixel of a resolution-scaled resolve.

The pass (src/graphics/shaders/resolve_downscale.cs.hlsl / .glsl; used by readback_resolve, by readback_resolve_rules (the vinyl readback) and by
resolve_unscaled_copy_rules) turns a resolution-scaled resolve back into the 1x data of the guest. The scaled data of a resolve is laid out by
XeTextureScaledTiledOffset of the xenia sources the SDK's shaders are built from (xenia-project/xenia 0b2ffa3, gpu/shaders/texture_address.xesli): host pixels are
kept in sub-units of N x 1 (N = 8 for 1 and 2 bytes a pixel, 4 for 4, 2 for 8, 1 for 16), column-major inside one guest unit of N pixels. This script

  1. transcribes XeTextureTiledOffset2D and XeTextureScaledTiledOffset (the reference),
  2. runs the pass's own address code: the text of XeDownscaleSourceOffset between the BEGIN and END lines of the .hlsl and of the .glsl, translated to Python
     (statements of uint arithmetic, if / else, return: nothing else is accepted), for every pixel size, every scale from 1x1 to 3x3 and both half-pixel
     settings, on textures of a few sizes,
  3. says, per pixel size and scale, for how many guest pixels
       - the old pass (xe_downscale_unit_layout = 0: the byte address of the guest pixel times scale_x * scale_y) reads the right host pixel,
       - the new pass (xe_downscale_unit_layout = 1) does; it must be every one, or the script ends with an error.
     The right host pixel is the top-left one of the guest pixel's block (scale_x * guest x, scale_y * guest y), or with the half-pixel setting the one at
     (scale_x / 2, scale_y / 2) inside the block.

  python -I downscale-layout-check.py [--sdk DIR]      (default: third_party/rexglue-sdk next to launcher/, else the current directory)

Result of 2026-10-28 on the shaders of the fork's patch rexglue-resolve-downscale-unit-layout.patch: see launcher/dev/vinyl-downscale-2026-10-28.md.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
M32 = 0xFFFFFFFF


# ----------------------------------------------------------------------------------------------------------------- the reference
def tiled_offset_2d(x, y, pitch_aligned, bpb_log2):
    # XeTextureTiledOffset2D
    macro = ((x >> 5) + (y >> 5) * (pitch_aligned >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def consecutive_log2(bpb_log2):
    # XeTextureTiledConsecutiveBlocksLog2
    return min(4 - bpb_log2, 3)


def scaled_offset(px, py, pitch_aligned, bpb_log2, sx, sy):
    # XeTextureScaledTiledOffset (2D): the byte offset of host pixel (px, py) in the scaled data
    uw = consecutive_log2(bpb_log2)
    x_sub = px >> uw
    ux, uy = x_sub // sx, py // sy
    unit_addr = tiled_offset_2d(ux << uw, uy, pitch_aligned, bpb_log2)
    sub_x, sub_y = x_sub - ux * sx, py - uy * sy
    return unit_addr * (sx * sy) + ((((sub_x * sy + sub_y) << uw) + (px & ((1 << uw) - 1))) << bpb_log2)


# ----------------------------------------------------------------------------------------------- the shaders' own text, run in Python
def extract_function(path):
    text = open(path, encoding="utf-8").read().replace("\r\n", "\n")
    a = text.index("// BEGIN XeDownscaleSourceOffset")
    b = text.index("// END XeDownscaleSourceOffset")
    return text[a:b]


def translate(source):
    """The function's text as a Python function. Accepts what the function uses and nothing else."""
    no_comments = "\n".join(line.split("//")[0] for line in source.split("\n"))
    head = re.match(r"\s*uint\s+(\w+)\s*\(([^)]*)\)\s*\{", no_comments)
    if not head:
        raise SystemExit("the function's head was not recognised")
    name = head.group(1)
    params = [p.split()[-1] for p in head.group(2).split(",")]
    body = no_comments[head.end():]
    body = body[:body.rindex("}")]
    body = re.sub(r"\b(\d+)u\b", r"\1", body)
    # statements and blocks (a chunk is followed by its terminator)
    tokens = [t.strip() for t in re.findall(r"[^;{}]+|[;{}]", body) if t.strip()]
    out = [f"def {name}({', '.join(params)}):"]
    indent = 1
    i = 0
    while i < len(tokens):
        t = tokens[i]
        nxt = tokens[i + 1] if i + 1 < len(tokens) else ""
        if t == "}":
            indent -= 1
        elif nxt == "{":
            header = " ".join(t.split())
            if header.startswith("if"):
                cond = header[2:].strip()
                cond = cond.replace("&&", " and ").replace("||", " or ")
                out.append("    " * indent + f"if {cond}:")
            elif header == "else":
                out.append("    " * indent + "else:")
            else:
                raise SystemExit(f"unsupported block header: {header}")
            indent += 1
            i += 1  # the '{'
        elif nxt == ";":
            stmt = " ".join(t.split())
            if stmt.startswith("return "):
                out.append("    " * indent + "return (" + stmt[len("return "):] + f") & {M32}")
            elif stmt.startswith("uint "):
                decl = stmt[len("uint "):]
                if "=" in decl:
                    lhs, rhs = decl.split("=", 1)
                    out.append("    " * indent + f"{lhs.strip()} = ({rhs.strip()}) & {M32}")
                else:
                    out.append("    " * indent + f"{decl.strip()} = 0")
            else:
                lhs, rhs = stmt.split("=", 1)
                out.append("    " * indent + f"{lhs.strip()} = ({rhs.strip()}) & {M32}")
            i += 1  # the ';'
        else:
            raise SystemExit(f"unsupported text: {t!r} ... {nxt!r}")
        i += 1
    namespace = {"min": min}
    exec("\n".join(out), namespace)
    return namespace[name], "\n".join(out)


# ------------------------------------------------------------------------------------------------------------------------------ the check
def check(fn, size_pitch, size_height, bpb_log2, sx, sy, half, unit_layout):
    """For every guest pixel of a texture of size_pitch x size_height: does fn read the scaled data at the right host pixel's offset? -> (right, total)"""
    pixel_size = 1 << bpb_log2
    tile_size_1x = 32 * 32 * pixel_size
    ox, oy = (sx >> 1, sy >> 1) if half and sx * sy > 1 else (0, 0)
    right = total = 0
    for gy in range(size_height):
        for gx in range(size_pitch):
            g_addr = tiled_offset_2d(gx, gy, size_pitch, bpb_log2)
            tile_index, in_tile = divmod(g_addr, tile_size_1x)
            pixel_index = in_tile // pixel_size
            want = scaled_offset(gx * sx + ox, gy * sy + oy, size_pitch, bpb_log2, sx, sy)
            got = fn(tile_index, pixel_index, sx, sy, bpb_log2, 1 if half else 0, unit_layout)
            right += got == want
            total += 1
    return right, total


def main():
    ap = argparse.ArgumentParser()
    default_sdk = os.path.normpath(os.path.join(HERE, "..", "..", "third_party", "rexglue-sdk"))
    ap.add_argument("--sdk", default=default_sdk if os.path.isdir(default_sdk) else ".")
    ap.add_argument("--quick", action="store_true", help="64x64 only")
    a = ap.parse_args()
    shaders = os.path.join(a.sdk, "src", "graphics", "shaders")
    funcs = {}
    for ext in ("hlsl", "glsl"):
        path = os.path.join(shaders, f"resolve_downscale.cs.{ext}")
        funcs[ext], _ = translate(extract_function(path))
    texts = {ext: extract_function(os.path.join(shaders, f"resolve_downscale.cs.{ext}")) for ext in ("hlsl", "glsl")}
    # the two shaders carry the same text apart from the comment about where the other copy is
    strip = lambda t: "\n".join(l for l in t.split("\n") if "the same text is in" not in l and "and is what launcher/dev" not in l)
    same_text = strip(texts["hlsl"]) == strip(texts["glsl"])
    print(f"the function's text is the same in the .hlsl and the .glsl: {same_text}")
    failures = 0 if same_text else 1
    sizes = [(64, 64)] if a.quick else [(64, 64), (128, 32), (32, 96)]
    names = {0: "1 byte ", 1: "2 bytes", 2: "4 bytes", 3: "8 bytes"}
    print("pixels read right (of all), old pass / new pass; half = the half-pixel setting; every size, every scale 1x1 .. 3x3 (a line per size, scale, setting)")
    for bpb_log2 in (0, 1, 2, 3):
        for sx in (1, 2, 3):
            for sy in (1, 2, 3):
                for half in (0, 1):
                    old_right = new_right = total = 0
                    for (w, h) in sizes:
                        r0, t = check(funcs["hlsl"], w, h, bpb_log2, sx, sy, half, 0)
                        r1, _ = check(funcs["hlsl"], w, h, bpb_log2, sx, sy, half, 1)
                        g1, _ = check(funcs["glsl"], w, h, bpb_log2, sx, sy, half, 1)
                        if g1 != r1:
                            print(f"  GLSL and HLSL differ at {names[bpb_log2]} {sx}x{sy} half={half}")
                            failures += 1
                        old_right += r0
                        new_right += r1
                        total += t
                    mark = "" if new_right == total else "   <-- NEW PASS WRONG"
                    if new_right != total:
                        failures += 1
                    if (sx, sy) in ((1, 1), (2, 2), (3, 3), (2, 1), (1, 2)) or mark:
                        print(f"  {names[bpb_log2]} a pixel, {sx}x{sy}, half={half}: old {old_right}/{total}, new {new_right}/{total}{mark}")
    # the old code path must be what it was: the original shader's expression
    for bpb_log2 in (0, 1, 2, 3):
        for sx, sy in ((2, 2), (3, 3), (2, 3)):
            for half in (0, 1):
                for tile_index, pixel_index in ((0, 0), (0, 1), (3, 777), (9, 1023)):
                    pixel_size = 1 << bpb_log2
                    tile_size_1x = 32 * 32 * pixel_size
                    ox, oy = (sx >> 1, sy >> 1) if half else (0, 0)
                    original = tile_index * tile_size_1x * sx * sy + pixel_index * pixel_size * sx * sy + (ox + oy * sx) * pixel_size
                    if funcs["hlsl"](tile_index, pixel_index, sx, sy, bpb_log2, half, 0) != original:
                        print("  the old path is not the original expression at", bpb_log2, sx, sy, half, tile_index, pixel_index)
                        failures += 1
    print("old path = the original shader's expression: checked")
    if failures:
        print(f"FAILED: {failures} problem(s)")
        sys.exit(1)
    print("OK: the new pass reads the right host pixel for every guest pixel tried")


if __name__ == "__main__":
    main()
