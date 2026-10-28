"""Checks, by arithmetic only (no GPU, no game), what the SDK's resolve_downscale pass reads from the scaled resolve buffer.

The pass (src/graphics/shaders/resolve_downscale.cs.hlsl, used by readback_resolve and by resolve_unscaled_copy_rules) maps a guest byte address `a` of a tiled
destination to the scaled byte address `a * scale_x * scale_y` ("pixel * pixel_size * scale_xy" in the shader) and copies the pixel there. The scaled layout is
XeTextureScaledTiledOffset of the xenia sources the SDK's shaders are built from (xenia-project/xenia 0b2ffa3, src/xenia/gpu/shaders/texture_address.xesli):
host pixels are kept in sub-units of N x 1 (N = 8 for 1 and 2 bytes a pixel, 4 for 4, 2 for 8), column-major inside one guest unit. This script transcribes that function
and XeTextureTiledOffset2D, builds the map from every host pixel to its scaled byte offset, and says where the pass's read lands for a 64x64 texture:

  python -I scaled-resolve-layout-check.py

Result (run 2026-10-28): with 8 bytes a pixel (the rain's k_32_32_FLOAT resolves) the pass reads the top-left host pixel of every block at 2x2 and a host pixel inside
the guest pixel's own block at 3x3; with 4 bytes a pixel (the custom vinyl texture, 8_8_8_8) at 2x2 half of the guest pixels get a pixel of a neighbour's block, at 3x3 a
quarter; with 1 and 2 bytes most do. The unit-based inverse (what a correct pass would read) matches the top-left host pixel for every size and scale tried.
"""


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
    # XeTextureScaledTiledOffset (2D)
    uw = consecutive_log2(bpb_log2)
    x_sub = px >> uw
    ux, uy = x_sub // sx, py // sy
    unit_addr = tiled_offset_2d(ux << uw, uy, pitch_aligned, bpb_log2)
    sub_x, sub_y = x_sub - ux * sx, py - uy * sy
    return unit_addr * (sx * sy) + ((((sub_x * sy + sub_y) << uw) + (px & ((1 << uw) - 1))) << bpb_log2)


def unit_inverse(g_addr, bpb_log2, sx, sy):
    # where the top-left host pixel of the guest pixel at tiled address g_addr is, from the unit layout
    uw = consecutive_log2(bpb_log2)
    n = 1 << uw
    unit_bytes = n << bpb_log2
    unit_base = g_addr & ~(unit_bytes - 1)
    u = (g_addr & (unit_bytes - 1)) >> bpb_log2
    s = (u * sx) >> uw
    q = (u * sx) & (n - 1)
    return unit_base * sx * sy + ((((s * sy) << uw) + q) << bpb_log2)


def check(bpb_log2, sx, sy, size=64):
    host_of = {}
    for hy in range(size * sy):
        for hx in range(size * sx):
            host_of[scaled_offset(hx, hy, size, bpb_log2, sx, sy)] = (hx, hy)
    top_left = inside = unit_ok = 0
    for gy in range(size):
        for gx in range(size):
            g_addr = tiled_offset_2d(gx, gy, size, bpb_log2)
            hx, hy = host_of[g_addr * sx * sy]
            top_left += (hx, hy) == (gx * sx, gy * sy)
            inside += hx // sx == gx and hy // sy == gy
            unit_ok += unit_inverse(g_addr, bpb_log2, sx, sy) == scaled_offset(gx * sx, gy * sy, size, bpb_log2, sx, sy)
    return top_left, inside, unit_ok


total = 64 * 64
for bpb_log2, name in ((0, "1 byte "), (1, "2 bytes"), (2, "4 bytes"), (3, "8 bytes")):
    for sx, sy in ((2, 2), (3, 3)):
        top_left, inside, unit_ok = check(bpb_log2, sx, sy)
        print(f"{name} a pixel, {sx}x{sy}: the pass reads the top-left host pixel for {top_left} of {total} guest pixels, a host pixel of the pixel's own block for {inside}; "
              f"the unit-based inverse is the top-left one for {unit_ok}")
