r"""Desk estimate (agent vram19): the host render targets a frame log implies, and their size at 1x, 2x and 3x.

Reads the D lines of a frame log (SDK frame-log patch: si = RB_SURFACE_INFO, ci = the four RB_COLORn_INFO, di = RB_DEPTH_INFO, cm = the colour write
mask, dc = RB_DEPTHCONTROL) and rebuilds the SDK's render target keys (RenderTargetCache::Update): pitch_tiles_at_32bpp = ceil((pitch << msaa_x_log2) / 80),
the key = (EDRAM base, pitch tiles, MSAA, storage format, colour or depth). The host render target of a key (D3D12RenderTargetCache::CreateRenderTarget,
RenderTargetCache::GetRenderTargetHeight) is key.GetWidth() * scale wide and tile_rows * (16 >> msaa_y_log2) * scale tall, with tile_rows = 2048 / pitch tiles
(2,048 = the console's EDRAM, whatever edram_tiles is), clamped to 8192 * scale rows; and has 1, 2 (native 2x MSAA) or 4 samples of the format's bytes.

    python -I rtcensus.py LOG [LOG ...]

What it does not know: base-address duplicates of the same key at another EDRAM base (counted as different keys, as the SDK does), the pause menu's
4x keys (not in the logs; added by hand in the note), host depth copies (unorm24 / float24), the TAA/FSR/HDR resources.
"""
import re
import sys
from collections import OrderedDict

COLOR_BYTES = {0: 4, 1: 4, 2: 4, 3: 8, 4: 4, 5: 8, 6: 4, 7: 8, 10: 4, 12: 8, 14: 4, 15: 8}
COLOR_NAME = {0: "8888", 1: "8888G", 2: "2101010", 3: "7e3", 4: "1616", 5: "16161616", 6: "1616F", 7: "16161616F", 10: "AS1010", 12: "7e3as16", 14: "32F",
              15: "3232F"}
STORAGE = {10: 2, 12: 3}   # GetStorageColorFormat: the AS_ variants share the base format's render target
DEPTH_BYTES = {0: 4, 1: 8}
DEPTH_NAME = {0: "D24S8", 1: "D24FS8"}


def key_size(kind, fmt, pitch_tiles, msaa, scale, rows_needed=None, cap=None):
    """(width, height, samples, bytes) of the host render target. rows_needed (pixels, 1x scale) given: the height class of the proposed fix, the needed tile
    rows rounded up to a power of two and at least 16 (never above the full height)."""
    x_log2 = 1 if msaa >= 2 else 0     # msaa: 0 = 1x, 1 = 2x, 2 = 4x
    y_log2 = 1 if msaa >= 1 else 0
    width = pitch_tiles * (80 >> x_log2) * scale
    max_height_scaled = min(8192 * scale, 16384)    # the guest limit scaled, and D3D12's 16,384 (GetMaxRenderTargetHeight)
    tile_rows = min(2048 // pitch_tiles, (max_height_scaled << y_log2) // (16 * scale))
    if cap:
        tile_rows = min(tile_rows, -(-cap // (16 >> y_log2)))
    if rows_needed:
        need_tile_rows = -(-int(rows_needed) // (16 >> y_log2))
        cls = 16
        while cls < need_tile_rows:
            cls *= 2
        tile_rows = min(tile_rows, cls)
    height = tile_rows * (16 >> y_log2) * scale
    samples = {0: 1, 1: 2, 2: 4}[msaa]
    per = DEPTH_BYTES[fmt] if kind == "d" else COLOR_BYTES[fmt]
    return width, height, samples, width * height * samples * per


def draw_rows(f):
    """Rows (pixels) a draw needs from its targets: the viewport's bottom if the viewport transform is on, else the window scissor's; None if neither says."""
    vte = int(f["vte"], 16)
    best = []
    try:
        vp = [float(x) for x in f["vp"].split(",")]
        if vte & 0xC:   # y scale and offset on
            best.append(vp[3] + abs(vp[2]))
    except (KeyError, ValueError):
        pass
    ws = f.get("ws", "").split(",")
    if len(ws) == 2:
        br_y = (int(ws[1], 16) >> 16) & 0x7FFF
        if 0 < br_y < 8192:
            best.append(br_y)
    return max(best) if best else None


def census(path):
    keys = OrderedDict()
    rows_needed = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("D "):
            continue
        f = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
        si = int(f["si"], 16)
        pitch = si & 0x3FFF
        msaa = (si >> 16) & 3
        x_log2 = 1 if msaa >= 2 else 0
        pitch_tiles = ((pitch << x_log2) + 79) // 80
        if not pitch_tiles:
            continue
        need = draw_rows(f)
        dc = int(f["dc"], 16)
        di = int(f["di"], 16)
        if dc & 3:
            k = ("d", di & 0xFFF, (di >> 16) & 1, pitch_tiles, msaa)
            keys[k] = keys.get(k, 0) + 1
            rows_needed[k] = max(rows_needed.get(k) or 0, need or 0) or None
        cm = int(f["cm"], 16)
        ci = [int(x, 16) for x in f["ci"].split(",")]
        for i in range(4):
            if cm & (0xF << (4 * i)):
                fmt = (ci[i] >> 16) & 0xF
                fmt = STORAGE.get(fmt, fmt)
                k = ("c", ci[i] & 0xFFF, fmt, pitch_tiles, msaa)
                keys[k] = keys.get(k, 0) + 1
                rows_needed[k] = max(rows_needed.get(k) or 0, need or 0) or None
    return keys, rows_needed


def main():
    for path in sys.argv[1:]:
        keys, need = census(path)
        print(f"== {path}: {len(keys)} keys")
        tot = {1: 0, 2: 0, 3: 0}
        totc = {1: 0, 2: 0, 3: 0}
        for k, draws in sorted(keys.items(), key=lambda kv: (kv[0][0], kv[0][1])):
            kind, base, fmt, pitch_tiles, msaa = k
            name = DEPTH_NAME[fmt] if kind == "d" else COLOR_NAME.get(fmt, str(fmt))
            sizes = {s: key_size(kind, fmt, pitch_tiles, msaa, s) for s in (1, 2, 3)}
            classes = {s: key_size(kind, fmt, pitch_tiles, msaa, s, need.get(k)) for s in (1, 2, 3)}
            for s in (1, 2, 3):
                tot[s] += sizes[s][3]
                totc[s] += classes[s][3]
            w2, h2, smp, b2 = sizes[2]
            print(f"  {kind} base {base:4d} {name:9s} pitch {pitch_tiles:3d} tiles {['1x', '2x', '4x'][msaa]} MSAA, {draws:4d} draws, needs {need.get(k)} rows: "
                  f"2x {sizes[2][3] / 2**20:6.1f} MB ({w2}x{h2}x{smp}) -> class {classes[2][3] / 2**20:6.1f} MB ({classes[2][0]}x{classes[2][1]})")
        print(f"  TOTAL host render targets now: 1x {tot[1] / 2**20:.0f} MB, 2x {tot[2] / 2**20:.0f} MB, 3x {tot[3] / 2**20:.0f} MB")
        print(f"  TOTAL with height classes:     1x {totc[1] / 2**20:.0f} MB, 2x {totc[2] / 2**20:.0f} MB, 3x {totc[3] / 2**20:.0f} MB")
        for cap in (768, 1024, 1280, 1536, 2048):
            t = {sc: sum(key_size(k[0], k[2], k[3], k[4], sc, None, cap)[3] for k in keys) for sc in (1, 2, 3)}
            print(f"  TOTAL with a {cap}-row cap:      1x {t[1] / 2**20:.0f} MB, 2x {t[2] / 2**20:.0f} MB, 3x {t[3] / 2**20:.0f} MB")


if __name__ == "__main__":
    main()
