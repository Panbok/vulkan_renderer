"""Download the mannequin's pinned third-party inputs and verify them.

    python3 tools/blender/mannequin/fetch_sources.py <sources-dir>

Reads sources.json beside this script and writes each file to
<sources-dir>/<path>. A file already present with the right SHA-256 is kept;
a mismatch after download fails the run, so the generator never builds from
unexpected data. A record with `extract` is a zip archive unpacked into
<sources-dir>/<extract>. Plain Python 3, no third-party packages.
"""

import hashlib
import json
import sys
import time
import urllib.request
import zipfile
from pathlib import Path


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(url, destination, attempts=3):
    temporary = destination.with_suffix(destination.suffix + ".part")
    # Some hosts refuse urllib's default agent string.
    request = urllib.request.Request(url, headers={"User-Agent": "vkr-fetch-sources/1"})
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(request, timeout=120) as response, open(temporary, "wb") as out:
                while True:
                    block = response.read(1 << 20)
                    if not block:
                        break
                    out.write(block)
            temporary.replace(destination)
            return
        except OSError as error:
            if attempt == attempts:
                raise RuntimeError(f"Download failed: {url}: {error}") from error
            time.sleep(2.0 * attempt)


def extract(archive, target):
    """Unpack members missing or changed in `target`, refusing paths that
    would land outside it."""
    target = target.resolve()
    with zipfile.ZipFile(archive) as bundle:
        for member in bundle.infolist():
            destination = (target / member.filename).resolve()
            if target not in destination.parents and destination != target:
                raise RuntimeError(f"{archive}: member {member.filename} leaves {target}")
            if member.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            if destination.is_file() and destination.stat().st_size == member.file_size:
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with bundle.open(member) as source, open(destination, "wb") as out:
                while True:
                    block = source.read(1 << 20)
                    if not block:
                        break
                    out.write(block)


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    root = Path(sys.argv[1])
    manifest = json.loads((Path(__file__).resolve().parent / "sources.json").read_text(encoding="utf-8"))
    fetched = 0
    for record in manifest["files"]:
        destination = root / record["path"]
        if not (destination.is_file() and sha256(destination) == record["sha256"]):
            destination.parent.mkdir(parents=True, exist_ok=True)
            fetch(record["url"], destination)
            actual = sha256(destination)
            if actual != record["sha256"]:
                destination.unlink()
                raise RuntimeError(f"{record['path']}: SHA-256 {actual} does not match {record['sha256']}")
            fetched += 1
        if "extract" in record:
            extract(destination, root / record["extract"])
    print(f"{len(manifest['files'])} sources ready in {root} ({fetched} downloaded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
