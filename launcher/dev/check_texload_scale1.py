r"""Checks that the guest addressing of the resolution-scaled load shaders at scale 1x1 equals the addressing of the unscaled ones.

texture_load_direct's shaders are built with XE_TEXTURE_LOAD_RESOLUTION_SCALED (XeTextureScaledTiledOffset and
XeTextureScaledRightSubUnitOffsetInConsecutivePair); the unscaled shaders use XeTextureTiledOffset2D / 3D and
XeTextureTiledOddConsecutiveBlocksOffset (xenia 0b2ffa3 texture_address.xesli and texture_load.xesli). This is a line by line
port of those functions (int32 / uint32 wrap-around included) and compares them on a grid. Used by the texture_load_direct_unscaled
patch, which loads unscaled resolved textures with the scaled direct shaders (push constants: scale 1, 1).

  python check_texload_scale1.py   (needs numpy) prints the cases and "ALL EQUAL" or the first differences, exit code 1 on a difference
"""
import sys
import numpy as np

M32 = 0xFFFFFFFF


def i32(a):
    """Wrap an int64 array to int32 values (kept in int64)."""
    a = np.asarray(a, dtype=np.int64) & M32
    return np.where(a >= 0x80000000, a - 0x100000000, a)


def u32(a):
    return np.asarray(a, dtype=np.int64) & M32


def sar(a, n):
    """Arithmetic shift right of an int32 value."""
    return np.asarray(a, dtype=np.int64) >> n


def tiled2d(x, y, pitch_aligned, bpb_log2):
    macro = i32(((x >> 5) + (y >> 5) * (pitch_aligned >> 5)) << (bpb_log2 + 7))
    micro = i32(((x & 7) + ((y & 0xE) << 2)) << bpb_log2)
    offset = i32(macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4))
    return i32(((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
               (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def tiled3d(x, y, z, pitch_aligned, height_aligned, bpb_log2):
    macro_outer = i32(((y >> 4) + (z >> 2) * (height_aligned >> 4)) * (pitch_aligned >> 5))
    macro = i32(((((x >> 5) + macro_outer) << (bpb_log2 + 6)) & 0xFFFFFFF) << 1)
    micro = i32((((x & 7) + ((y & 6) << 2)) << (bpb_log2 + 6)) >> 6)
    offset_outer = ((y >> 3) + (z >> 2)) & 1
    offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1)
    offset2 = i32(((macro + (micro & ~15)) << 1) + (micro & 15) + ((z & 3) << (bpb_log2 + 6)) + ((y & 1) << 4))
    address = (offset1 & 1) << 3
    address = address + ((offset2 >> 6) & 7)
    address = address << 3
    address = address + (offset1 & ~1)
    address = address << 2
    address = i32(address + (offset2 & ~511))
    address = address << 3
    address = i32(address + (offset2 & 63))
    return address


def consecutive_log2(bpb_log2):
    return min(4 - bpb_log2, 3)


def odd_offset(bpb_log2):
    return 32 if bpb_log2 >= 2 else 64


def scaled_offset(is_3d, x, y, z, pitch_aligned, height_aligned, bpb_log2, sx, sy):
    """XeTextureScaledTiledOffset, unsigned arithmetic."""
    ul = consecutive_log2(bpb_log2)
    x_sub = u32(x) >> ul
    xu = x_sub // sx
    yu = u32(y) // sy
    ox = u32(xu << ul)
    if is_3d:
        addr = u32(tiled3d(ox, yu, z, pitch_aligned, height_aligned, bpb_log2))
    else:
        addr = u32(tiled2d(ox, yu, pitch_aligned, bpb_log2))
    sub_x = x_sub - xu * sx
    sub_y = u32(y) - yu * sy
    return u32(addr * (sx * sy) + (((((sub_x * sy + sub_y) << ul) + (u32(x) & ((1 << ul) - 1))) << bpb_log2)))


def scaled_right(x, bpb_log2, sx, sy):
    cons = odd_offset(bpb_log2)
    if sx > 1:
        raise SystemExit("only used with scale 1")
    return cons * sy


def main():
    bad = 0
    cases = 0
    for bpb_log2 in (2, 3):
        for pitch in (32, 64, 96, 128, 640, 1024, 1280, 1536, 2048, 4096):
            w = min(pitch, 2048)
            for h in (32, 720, 1280, 2048):
                ys, xs = np.mgrid[0:h, 0:w]
                xs = xs.astype(np.int64)
                ys = ys.astype(np.int64)
                # 2D tiled, every block position.
                a = tiled2d(xs, ys, pitch, bpb_log2)
                b = scaled_offset(False, xs, ys, 0, pitch, 0, bpb_log2, 1, 1)
                cases += 1
                if not np.array_equal(u32(a), b):
                    bad += 1
                    d = np.argwhere(u32(a) != b)[:3]
                    print(f"2D DIFF bpb_log2={bpb_log2} pitch={pitch} h={h}: first at (y,x)={d.tolist()}")
                # 3D tiling of a 2D texture (z = 0), height_aligned = h rounded to 32.
                ha = (h + 31) // 32 * 32
                a3 = tiled3d(xs, ys, 0, pitch, ha, bpb_log2)
                b3 = scaled_offset(True, xs, ys, 0, pitch, ha, bpb_log2, 1, 1)
                cases += 1
                if not np.array_equal(u32(a3), b3):
                    bad += 1
                    d = np.argwhere(u32(a3) != b3)[:3]
                    print(f"3D DIFF bpb_log2={bpb_log2} pitch={pitch} h={h}: first at (y,x)={d.tolist()}")
        # The right-hand 16 bytes: the shaders load at x and at x + (16 >> bpb_log2) blocks; the first address is the tiled
        # offset of x, the second is the first plus the odd-consecutive offset (unscaled and scaled at 1x1 alike). Check that the
        # real address of the second group equals that sum, for every x multiple of 2 * (16 >> bpb_log2).
        per16 = 16 >> bpb_log2
        for pitch in (128, 1280, 2048):
            ys, xs = np.mgrid[0:256, 0:pitch:2 * per16]
            xs = xs.astype(np.int64)
            ys = ys.astype(np.int64)
            first = tiled2d(xs, ys, pitch, bpb_log2)
            second = tiled2d(xs + per16, ys, pitch, bpb_log2)
            cases += 1
            if not np.array_equal(u32(second - first), np.full(first.shape, odd_offset(bpb_log2), dtype=np.int64)):
                bad += 1
                print(f"RIGHT DIFF bpb_log2={bpb_log2} pitch={pitch}")
            cases += 1
            if scaled_right(0, bpb_log2, 1, 1) != odd_offset(bpb_log2):
                bad += 1
                print(f"RIGHT (scaled) DIFF bpb_log2={bpb_log2}")
    print(f"{cases} cases, {bad} different")
    print("ALL EQUAL" if not bad else "DIFFERENT")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
