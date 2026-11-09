#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the pinned, free DCMTK SDK and loopback test utilities."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from dependency_sources import DCMTK, fetch
from zipfile import ZipFile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True, help="SDK output outside the source checkout")
    parser.add_argument("--archive", type=Path, help="Use an existing verified DCMTK source ZIP")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    archive = fetch(DCMTK, args.archive or work / DCMTK.name, download=args.archive is None)
    source = work / DCMTK.name.removesuffix(".zip")
    if not source.exists():
        with tempfile.TemporaryDirectory(prefix="dcmtk-source-", dir=work) as staging, ZipFile(archive) as contents:
            contents.extractall(staging)
            (Path(staging) / source.name).replace(source)
    build = work / "build"
    sdk = work / "sdk"
    modules = "ofstd;oflog;oficonv;dcmdata;dcmimgle;dcmimage;dcmjpeg;dcmjpls;dcmtls;dcmnet;dcmwlm;dcmqrdb"
    optional = ("TIFF", "PNG", "XML", "ZLIB", "OPENSSL", "SNDFILE", "ICONV", "OPENJPEG", "DOXYGEN", "WRAP")
    subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={sdk}",
                    f"-DDCMTK_MODULES={modules}", "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF",
                    "-DDCMTK_DEFAULT_DICT=builtin", "-DDCMTK_ENABLE_CHARSET_CONVERSION=oficonv",
                    "-DDCMTK_ENABLE_BUILTIN_OFICONV_DATA=ON",
                    *(f"-DDCMTK_WITH_{feature}=OFF" for feature in optional)], check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel", str(args.jobs), "--target", "install"], check=True)
    print(f"DCMTK SDK and utilities: {sdk}")


if __name__ == "__main__":
    main()

