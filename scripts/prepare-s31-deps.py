#!/usr/bin/env python3
"""Restore the registry packages used by the offline S31 component list.

Versions match dependencies.lock. Existing packages are never overwritten.
"""

import hashlib
import io
from pathlib import Path
import tempfile
from urllib.request import urlopen
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = (
    ("usb_host_uac", "1.2.0", "1.2.0",
     "a627f895edae4429beedc040ed5b2d18f6466ca66615e9c13fff25b016cd9c1f"),
    ("cmake_utilities", "0.5.3", "0.5.3",
     "b80ba7ac010bf76c392021586a3e0e0e7ed9c52fcdb970145d2c42dbb198b136"),
    ("libjpeg-turbo", "3.1.1~1", "3.1.1_1",
     "1e10f288db1c9c9089114ad0f5cbd56a1ac44499272c0e895efd4dfd8736d3ae"),
)
FILES = (
    ("espressif__esp_extractor/include/impl/esp_mjpeg_extractor.h",
     "https://components-file.espressif.com/components/espressif/"
     "esp_extractor/1.0.5/espressif__esp_extractor-v1.0.5.zip",
     "include/impl/esp_mjpeg_extractor.h",
     "47a79fa4cbd6413f1c5d1ff1e394ecfd142503281bc84d50ad4719c1ea21a7a6"),
)


def download(url, digest, member=None):
    with urlopen(url, timeout=120) as response:
        data = response.read()
    if member is not None:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            data = archive.read(member)
    if hashlib.sha256(data).hexdigest() != digest:
        raise RuntimeError(f"SHA256 mismatch: {url}")
    return data


def main():
    parent = ROOT / "managed_components"
    parent.mkdir(exist_ok=True)
    for name, version, archive_version, digest in PACKAGES:
        destination = parent / f"espressif__{name}"
        if (destination / "CMakeLists.txt").is_file():
            print(f"Already present: {destination.name}")
            continue
        if destination.exists():
            raise RuntimeError(f"Incomplete component: {destination}; inspect it before retrying")
        url = ("https://components-file.espressif.com/components/espressif/"
               f"{name}/{version}/espressif__{name}-v{archive_version}.zip")
        print(f"Downloading {name} {version}", flush=True)
        data = download(url, digest)
        with tempfile.TemporaryDirectory(dir=parent) as temporary:
            staging = Path(temporary) / "component"
            staging.mkdir()
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                for member in archive.infolist():
                    target = (staging / member.filename).resolve()
                    if not target.is_relative_to(staging.resolve()):
                        raise RuntimeError(f"Unsafe archive path: {member.filename}")
                archive.extractall(staging)
            if not (staging / "CMakeLists.txt").is_file():
                raise RuntimeError(f"Missing CMakeLists.txt in {name}")
            staging.rename(destination)
        print(f"Restored: {destination.name}")
    for relative_path, url, member, digest in FILES:
        destination = parent / relative_path
        if destination.is_file():
            continue
        print(f"Restoring {relative_path}", flush=True)
        data = download(url, digest, member)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)


if __name__ == "__main__":
    main()
