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


def import_sibling(name):
    """Imports a script of this folder as a module (python -I does not put the folder on the path)."""
    here = str(pathlib.Path(__file__).resolve().parent)
    if here not in sys.path:
        sys.path.insert(0, here)
    return __import__(name)
