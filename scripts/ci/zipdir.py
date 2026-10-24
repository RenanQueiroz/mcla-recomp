r"""A deterministic zip of a folder: python zipdir.py <folder> <zip> [--source-date-epoch N]

Files in sorted order with the folder's contents at the top of the zip, fixed times, fixed modes
(executable for .exe and .dll), deflate level 9: the same folder gives the same zip, byte for byte.
Also imported by make-sdk-bundle.py (write_zip).
"""
import argparse
import hashlib
import pathlib
import time
import zipfile

DEFAULT_EPOCH = 1767225600  # 2026-01-01


def write_zip(folder, target, epoch=DEFAULT_EPOCH):
    """Zips `folder` into `target`; returns (file count, unpacked bytes)."""
    folder, target = pathlib.Path(folder).resolve(), pathlib.Path(target).resolve()
    stamp = time.gmtime(max(int(epoch), 315532800))[:6]
    files = sorted(p for p in folder.rglob("*") if p.is_file())
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        target.unlink()
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in files:
            info = zipfile.ZipInfo(p.relative_to(folder).as_posix(), date_time=stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (0o100755 if p.suffix.lower() in (".exe", ".dll") else 0o100644) << 16
            z.writestr(info, p.read_bytes(), compresslevel=9)
    return len(files), sum(p.stat().st_size for p in files)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder")
    ap.add_argument("zip")
    ap.add_argument("--source-date-epoch", type=int, default=DEFAULT_EPOCH)
    a = ap.parse_args()
    count, size = write_zip(a.folder, a.zip, a.source_date_epoch)
    target = pathlib.Path(a.zip)
    print(f"zipdir: {target.name}: {count} files, {size / 1e6:.1f} MB unpacked, "
          f"{target.stat().st_size / 1e6:.1f} MB zipped, sha256 {hashlib.sha256(target.read_bytes()).hexdigest()}")
