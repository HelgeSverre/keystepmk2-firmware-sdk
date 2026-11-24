#!/usr/bin/env python3
"""Download the published KeyStep mk2 firmware packages and unpack them.

A .ks2fw file is a zip (stored, not compressed) holding info.json and one image. Packages come from
Arturia's public download endpoint, the one the "Download" buttons on the product's resources page
use. Each is unpacked to firmware/<version>/ and recorded in firmware/listing.json.

Run from the repository root:  uv run python tools/fwfetch.py
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import urllib.request
import zipfile
from pathlib import Path

BASE = "https://www.arturia.com/api/download/products/{product}/firmware/keystep_mk2__{version}.ks2fw"
PAGE = "https://www.arturia.com/products/hybrid-synths/keystep-mk2/resources"
# Versions and release dates as listed on the resources page (checked 2026-11-22).
RELEASES = {
    "1.0.1": "2025-11-03",
    "1.0.2": "2025-11-14",
    "1.0.3": "2026-02-26",
    "1.0.4": "2026-02-26",
}
USER_AGENT = "Mozilla/5.0 (keystepmk2-firmware-sdk fwfetch)"
ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def unpack(package: bytes, version: str) -> dict:
    """Writes the package and its members under firmware/ and returns its listing entry."""
    archive = zipfile.ZipFile(io.BytesIO(package))
    names = archive.namelist()
    if "info.json" not in names:
        raise ValueError("no info.json in package")
    info = json.loads(archive.read("info.json"))
    target = FIRMWARE / version
    target.mkdir(parents=True, exist_ok=True)
    members = []
    for name in names:
        if "/" in name or "\\" in name or name.startswith("."):
            raise ValueError(f"unexpected member name {name!r}")
        data = archive.read(name)
        (target / name).write_bytes(data)
        members.append({"name": name, "size": len(data), "sha256": sha256(data)})
    return {"build": info.get("version_number"), "built": info.get("date"), "info": info, "members": members}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("versions", nargs="*", default=list(RELEASES), help="versions to fetch (default: all known)")
    parser.add_argument("--product", default="keystep-mk2", help="product slug in the download URL")
    args = parser.parse_args()

    FIRMWARE.mkdir(exist_ok=True)
    listing_path = FIRMWARE / "listing.json"
    listing = json.loads(listing_path.read_text()) if listing_path.exists() else {"source": PAGE, "packages": {}}
    failed = 0
    for version in args.versions:
        url = BASE.format(product=args.product, version=version.replace(".", "_"))
        try:
            package = download(url)
            entry = unpack(package, version)
        except Exception as error:  # noqa: BLE001 - report and carry on with the other versions
            print(f"{version}: FAILED {error}", file=sys.stderr)
            failed += 1
            continue
        name = f"keystep_mk2__{version.replace('.', '_')}.ks2fw"
        (FIRMWARE / name).write_bytes(package)
        entry = {
            "file": name,
            "url": url,
            "released": RELEASES.get(version),
            "size": len(package),
            "sha256": sha256(package),
            **entry,
        }
        listing["packages"][version] = entry
        image = max(entry["members"], key=lambda m: m["size"])
        print(
            f"{version}: build {entry['build']}, package {len(package)} bytes, image {image['name']} {image['size']} bytes"
        )
    listing_path.write_text(json.dumps(listing, indent=2) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
