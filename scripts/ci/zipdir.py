r"""A deterministic zip of a folder: python zipdir.py <folder> <zip> [--source-date-epoch N]

Files in sorted order with the folder's contents at the top of the zip, fixed times, fixed modes
(executable for .exe and .dll), deflate level 9: the same folder gives the same zip, byte for byte.
"""
import hashlib
import pathlib
import sys
import time
import zipfile

args = [a for a in sys.argv[1:] if not a.startswith("--")]
epoch = int(sys.argv[sys.argv.index("--source-date-epoch") + 1]) if "--source-date-epoch" in sys.argv else 1767225600
if "--source-date-epoch" in sys.argv:
    args.remove(sys.argv[sys.argv.index("--source-date-epoch") + 1])
if len(args) != 2:
    sys.exit(__doc__)
folder, target = pathlib.Path(args[0]).resolve(), pathlib.Path(args[1]).resolve()
stamp = time.gmtime(max(epoch, 315532800))[:6]
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
print(f"zipdir: {target.name}: {len(files)} files, {sum(p.stat().st_size for p in files) / 1e6:.1f} MB unpacked, "
      f"{target.stat().st_size / 1e6:.1f} MB zipped, sha256 {hashlib.sha256(target.read_bytes()).hexdigest()}")
