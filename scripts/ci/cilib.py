r"""Small helpers the release scripts share (imported; run nothing)."""
import hashlib
import pathlib
import sys


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sorted_patches(patches):
    """The patches of an SDK's manifest ({"name", "sha256"}), by name, ordinally (by code point:
    the same as scripts/make_pc.ps1's StringComparer.Ordinal for the ASCII names of patches/)."""
    return sorted(patches, key=lambda p: p["name"])


def patches_digest(patches):
    """The SHA-256 (64 hex digits) of the lines "<name> <sha256 of the patch>\\n" of the patches,
    sorted by name. The same patches give the same digest on any PC; the first 12 digits are in the
    names of the SDK bundle and of its source archive, all 64 are in release.json."""
    lines = "".join(f"{p['name']} {p['sha256']}\n" for p in sorted_patches(patches))
    return hashlib.sha256(lines.encode("ascii")).hexdigest()


def stem(sdk_commit, digest):
    """`<SDK commit, 8 digits>-<patches digest, 12 digits>`, as the bundle and the source archive are named."""
    return f"{sdk_commit[:8]}-{digest[:12]}"


def read_installer_header(path):
    """Reads the header of MCLA-Setup.exe's payload from the file (launcher/installer/Payload.cs):
    the installer program, then "MCLAPK1" + NUL, an int32 meta length and the meta ("key=value" lines),
    the Brotli stream of the launcher's files, and at the very end "MCLAPAY1" and the payload's length
    (int64). Returns the meta as a dict of strings; raises ValueError if the file is not that."""
    path = pathlib.Path(path)
    size = path.stat().st_size
    with path.open("rb") as f:
        if f.read(2) != b"MZ":
            raise ValueError(f"{path.name} is not a Windows program")
        f.seek(-16, 2)
        footer = f.read(16)
        if footer[:8] != b"MCLAPAY1":
            raise ValueError(f"{path.name} has no payload (no MCLAPAY1 footer)")
        payload_length = int.from_bytes(footer[8:], "little")
        start = size - 16 - payload_length
        if payload_length < 1_000_000 or start < 100_000:
            raise ValueError(f"{path.name}: the payload length is not believable")
        f.seek(start)
        head = f.read(12)
        if head[:7] != b"MCLAPK1" or head[7:8] != b"\x00":
            raise ValueError(f"{path.name}: the payload has the wrong header")
        meta_length = int.from_bytes(head[8:], "little")
        if not 0 <= meta_length <= 1 << 16:
            raise ValueError(f"{path.name}: the payload header's length is not believable")
        meta_text = f.read(meta_length).decode("utf-8")
    return dict(line.split("=", 1) for line in meta_text.splitlines() if "=" in line)


def check_installer_header(path, tag=None, commit=None, version=None, zip_files=None):
    """read_installer_header plus the checks of a release: every key is in the file itself (not
    supplied by the installer's own defaults), the launcher program is MclaLauncher.exe, and the tag,
    commit, version and the number of files are what is expected (when given). Returns the meta."""
    meta = read_installer_header(path)
    missing = [k for k in ("version", "tag", "commit", "exe", "files", "bytes") if k not in meta]
    if missing:
        raise ValueError(f"the payload header lacks {', '.join(missing)}: it says {meta}")
    for key, want in (("tag", tag), ("commit", commit), ("version", version), ("exe", "MclaLauncher.exe")):
        if want is not None and meta[key] != want:
            raise ValueError(f"the payload says {key}={meta[key]!r}, not {want!r}")
    if not meta["files"].isdigit() or int(meta["files"]) < 100 or not meta["bytes"].isdigit():
        raise ValueError("the payload header does not give the launcher's files")
    if zip_files is not None and int(meta["files"]) != zip_files:
        raise ValueError(f"the launcher zip has {zip_files} files, the installer's payload {meta['files']}")
    return meta


def import_sibling(name):
    """Imports a script of this folder as a module (python -I does not put the folder on the path)."""
    here = str(pathlib.Path(__file__).resolve().parent)
    if here not in sys.path:
        sys.path.insert(0, here)
    return __import__(name)
