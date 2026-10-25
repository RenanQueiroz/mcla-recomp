r"""Reads MCLA-Setup.exe's payload header from the file, the way the release's assemble step does, and
checks it: python check-installer.py <MCLA-Setup.exe> [--tag T] [--commit C] [--version V] [--launcher-zip Z | --files N]

build-installer.ps1 runs this on every exe it makes (CI and the release), so that what assemble-release.py
will read from the exe is known to be readable before the release's last jobs run. (A header that only the
installer program itself can read, because it fills in a default the file does not hold, once failed a release run
at its last-but-two job.) Prints the header; exits 1 with the reason if it is not right.
"""
import argparse
import pathlib
import sys
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cilib  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument("exe")
ap.add_argument("--tag")
ap.add_argument("--commit")
ap.add_argument("--version")
ap.add_argument("--launcher-zip")
ap.add_argument("--files", type=int)
a = ap.parse_args()

zip_files = a.files
if a.launcher_zip:
    with zipfile.ZipFile(a.launcher_zip) as z:
        zip_files = len(z.namelist())
try:
    meta = cilib.check_installer_header(a.exe, tag=a.tag, commit=a.commit, version=a.version, zip_files=zip_files)
except ValueError as e:
    sys.exit(f"check-installer: {e}")
print("check-installer: " + ", ".join(f"{k}={v}" for k, v in meta.items()))
