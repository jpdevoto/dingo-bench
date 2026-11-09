# SPDX-License-Identifier: GPL-3.0-only
"""Pinned upstream archives used by the build and distribution tools."""
from dataclasses import dataclass
from hashlib import file_digest
from pathlib import Path
import shutil
import tempfile
from urllib.request import urlopen


@dataclass(frozen=True)
class Source:
    name: str
    url: str
    sha256: str


QT_VERSION = "6.11.3"
DCMTK_VERSION = "3.7.1"
DCMTK_REVISION = "108acdae424d6999b2e398243938d8c563315ecf"
DCMTK = Source(f"dcmtk-{DCMTK_REVISION}.zip",
               f"https://codeload.github.com/DCMTK/dcmtk/zip/{DCMTK_REVISION}",
               "dc94b4d348cbeaa502e9c2a30d28ae1d3da8907946be16d56778224e4aa4d132")


def qt_source(module, sha256):
    name = f"{module}-everywhere-src-{QT_VERSION}.tar.xz"
    series = QT_VERSION.rsplit(".", 1)[0]
    return Source(name, f"https://download.qt.io/archive/qt/{series}/{QT_VERSION}/submodules/{name}", sha256)


QTBASE = qt_source("qtbase", "46de0710b594945a5d446ae44354daa386141c6903c53781b2ca547bc62f8f8f")
QTSVG = qt_source("qtsvg", "9bdf6fa4b5a2d91c3eaa2edd12263fdff0ae19d0ac25d3c99946cd4189a07123")


def digest(path: Path):
    with path.open("rb") as stream:
        return file_digest(stream, "sha256").hexdigest()


def fetch(source: Source, path: Path, *, download=True):
    if path.exists():
        if digest(path) != source.sha256:
            raise ValueError(f"SHA-256 mismatch: {path}")
        return path
    if not download:
        raise FileNotFoundError(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="download-", dir=path.parent) as staging:
        partial = Path(staging) / source.name
        with urlopen(source.url, timeout=60) as response, partial.open("xb") as output:
            shutil.copyfileobj(response, output)
        if digest(partial) != source.sha256:
            raise ValueError(f"SHA-256 mismatch: {source.url}")
        partial.replace(path)
    return path

