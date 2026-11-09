#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Stage and audit the Windows x64 portable ZIP and its corresponding source ZIP."""
import argparse
import json
import os
import posixpath
from pathlib import Path, PurePosixPath
import re
import runpy
import shutil
import subprocess
import tarfile
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from dependency_sources import DCMTK, QTBASE, QTSVG, QT_VERSION, digest, fetch

PLUGINS = ("platforms/qwindows.dll", "platforms/qoffscreen.dll", "styles/qmodernwindowsstyle.dll",
           "iconengines/qsvgicon.dll", "imageformats/qsvg.dll")
CRT = re.compile(r"(?i)(msvcp140|vcruntime140|concrt140|vccorlib140)[a-z_0-9]*\.dll")
DLL = re.compile(r"^\s+([A-Za-z0-9_.-]+\.dll)\s*$", re.I | re.M)


def command(*args):
    return subprocess.check_output(args, text=True, errors="replace")


def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def portable_runtime(stage, qt, redist):
    binary_paths = [stage / name for name in ("DingoBench.exe", "DingoBenchReceiver.exe", "wlmscpfs.exe")]
    for plugin in PLUGINS:
        copy(qt / "plugins" / plugin, stage / plugin)
        binary_paths.append(stage / plugin)
    system = Path(os.environ["SystemRoot"]) / "System32"
    examined = set()
    audit = []
    for binary in binary_paths:
        if binary in examined:
            continue
        examined.add(binary)
        if not re.search(r"\b8664 machine \(x64\)", command("dumpbin", "/nologo", "/headers", str(binary))):
            raise ValueError(f"Expected an x64 PE: {binary}")
        for name in dict.fromkeys(DLL.findall(command("dumpbin", "/nologo", "/dependents", str(binary)))):
            lower = name.lower()
            if CRT.fullmatch(name) or lower.startswith("qt6"):
                supplier = redist if CRT.fullmatch(name) else qt / "bin"
                if re.search(r"(?i)(?:qt6.*d|(?:msvcp|vcruntime)140d)\.dll$", name):
                    raise ValueError(f"Debug dependency: {name}")
                target = stage / name
                if not target.exists():
                    copy(supplier / name, target)
                binary_paths.append(target)
                origin = "MSVC app-local runtime" if supplier == redist else "Qt"
            elif lower.startswith(("api-ms-win-", "ext-ms-win-")) or (system / name).is_file():
                origin = "Windows"
            else:
                raise ValueError(f"Unresolved dependency of {binary.name}: {name}")
            audit.append(f"{binary.relative_to(stage).as_posix()}: {name} [{origin}]")
    (stage / "DEPENDENCIES.txt").write_text("\n".join(sorted(audit)) + "\n", encoding="utf-8")
    (stage / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\n", encoding="utf-8")


def qt_notices(archive, directory):
    needed = set()
    with tarfile.open(archive, "r|xz") as source:
        for member in source:
            relative = PurePosixPath(member.name)
            if not member.isfile() or relative.name != "qt_attribution.json":
                continue
            data = source.extractfile(member).read()
            (directory / relative).parent.mkdir(parents=True, exist_ok=True)
            (directory / relative).write_bytes(data)
            entries = json.loads(data, strict=False)
            for entry in entries if isinstance(entries, list) else [entries]:
                for field in ("LicenseFile", "LicenseFiles", "CopyrightFile"):
                    names = entry.get(field) or []
                    for name in [names] if isinstance(names, str) else names:
                        path = PurePosixPath(posixpath.normpath(str(relative.parent / name)))
                        if path.parts[0] == relative.parts[0]:
                            needed.add(path)
    with tarfile.open(archive, "r|xz") as source:
        for member in source:
            relative = PurePosixPath(member.name)
            if member.isfile() and (relative in needed or "LICENSES" in relative.parts):
                target = directory / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.extractfile(member).read())


def source_files(root):
    publisher = root / "publish.py"
    if publisher.is_file():
        paths = runpy.run_path(str(publisher))["approved_paths"]()
    elif (root / "SOURCE-FILES.txt").is_file():
        paths = [Path(line) for line in (root / "SOURCE-FILES.txt").read_text(encoding="utf-8").splitlines() if line]
    else:
        tracked = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"])
        paths = [Path(name) for name in tracked.decode("utf-8").split("\0") if name]
    for relative in paths:
        if relative.is_absolute() or ".." in relative.parts or any(
                part.lower() in {"build", "temp", "dist", "test-artifacts", "github", "logs", ".git", "__pycache__"}
                or part.lower().startswith(("build-", "cmake-build-")) for part in relative.parts
        ) or relative.suffix.lower() in {".dll", ".exe", ".lib", ".zip", ".log", ".dcm", ".pdb"}:
            raise ValueError(f"Ineligible source archive entry: {relative}")
        path = root / relative
        if not path.is_file() or path.resolve() != path:
            raise ValueError(f"Source archive entry is missing or redirected: {relative}")
        yield relative.as_posix(), path


def write_zip(path, entries):
    with ZipFile(path, "x", compression=ZIP_DEFLATED, compresslevel=6) as output:
        for name, source in sorted(entries):
            info = ZipInfo(name)
            info.compress_type = ZIP_DEFLATED
            with output.open(info, "w") as dest, source.open("rb") as data:
                shutil.copyfileobj(data, dest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--qt", required=True, type=Path)
    parser.add_argument("--dcmtk-work", required=True, type=Path)
    parser.add_argument("--downloads", required=True, type=Path, help="Directory for verified upstream archives")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--redist", type=Path, help="Microsoft.VC143.CRT x64 directory from Visual Studio")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if any(parent.name.lower() == "github" or (parent / ".git").exists() for parent in (output, *output.parents)):
        parser.error("Package output must be outside Git checkouts.")
    output.mkdir(parents=True, exist_ok=True)
    args.downloads = args.downloads.resolve()
    if not args.redist and not os.environ.get("VCToolsRedistDir"):
        parser.error("Run in an x64 Visual Studio developer shell or set --redist.")
    redist = args.redist or Path(os.environ["VCToolsRedistDir"]) / "x64/Microsoft.VC143.CRT"
    if command(str(args.qt.resolve() / "bin/qmake.exe"), "-query", "QT_VERSION").strip() != QT_VERSION:
        raise ValueError(f"Portable packaging requires Qt {QT_VERSION}.")
    cache = (args.build / "CMakeCache.txt").read_text(encoding="utf-8")
    def cache_path(variable):
        value = re.search(rf"^{variable}:[^=\n]+=(.+)$", cache, re.M)
        if not value:
            raise ValueError(f"Missing CMake cache entry: {variable}")
        return Path(value.group(1)).resolve()

    if cache_path("CMAKE_HOME_DIRECTORY") != root:
        raise ValueError("Run packaging from the exact source tree used by the Release build.")
    if not cache_path("Qt6_DIR").is_relative_to(args.qt.resolve()):
        raise ValueError("Qt SDK path differs from the SDK used by the build.")
    if not cache_path("DCMTK_DIR").is_relative_to(args.dcmtk_work.resolve() / "sdk"):
        raise ValueError("DCMTK SDK path differs from the SDK used by the build.")
    if "CMAKE_BUILD_TYPE:STRING=Release\n" not in cache:
        raise ValueError("Portable packaging requires a Release build.")
    version = re.search(r"^CMAKE_PROJECT_VERSION:STATIC=(\d+\.\d+\.\d+)$", cache, re.M)
    if not version:
        raise ValueError("CMake project version unavailable.")
    name = f"DingoBench-{version.group(1)}-windows-x64"
    binary_zip = output / f"{name}.zip"
    source_zip = output / f"{name}-source.zip"
    dcmtk_archive = args.dcmtk_work / DCMTK.name
    if not dcmtk_archive.exists():
        dcmtk_archive = args.downloads / DCMTK.name
    upstream = {DCMTK: fetch(DCMTK, dcmtk_archive), **{
        source: fetch(source, args.downloads / source.name) for source in (QTBASE, QTSVG)
    }}

    with tempfile.TemporaryDirectory(prefix="dingobench-package-", dir=output) as working:
        stage = Path(working)
        subprocess.run(["cmake", "--install", str(args.build.resolve()), "--prefix", str(stage)], check=True)
        copy(args.dcmtk_work / "sdk/bin/wlmscpfs.exe", stage / "wlmscpfs.exe")
        copy(root / "docs/PORTABLE.md", stage / "README.md")
        copy(root / "docs/licenses/README.md", stage / "licenses/THIRD-PARTY.md")
        materials = stage / "share/licenses/DingoBench/MaterialSymbols"
        shutil.move(materials, stage / "licenses/MaterialSymbols")
        for parent in (materials.parent, materials.parent.parent, materials.parent.parent.parent):
            parent.rmdir()
        licenses = stage / "licenses"
        for source in (QTBASE, QTSVG):
            qt_notices(upstream[source], licenses / "Qt")
        with ZipFile(upstream[DCMTK]) as dcmtk:
            (licenses / "DCMTK-COPYRIGHT.txt").write_bytes(
                dcmtk.read(f"{DCMTK.name.removesuffix('.zip')}/COPYRIGHT"))
        for sbom in ("qtbase", "qtsvg"):
            copy(args.qt / f"sbom/{sbom}-{QT_VERSION}.spdx.json", licenses / f"{sbom}-{QT_VERSION}.spdx.json")
        portable_runtime(stage, args.qt, redist)
        entries = sorted(source_files(root))
        source_entries = [(f"DingoBench-source/{entry}", path) for entry, path in entries]
        source_entries += [(f"upstream/{source.name}", path) for source, path in upstream.items()]
        manifest = stage / "SOURCE-FILES.txt"
        manifest.write_text("\n".join(entry for entry, _ in entries) + "\n", encoding="utf-8")
        source_entries.append(("DingoBench-source/SOURCE-FILES.txt", manifest))
        staged_source = stage / source_zip.name
        write_zip(staged_source, source_entries)
        manifest.unlink()
        (stage / "SOURCE.txt").write_text(
            f"Corresponding source: {source_zip.name}\nSHA-256: {digest(staged_source)}\n"
            "Distribute this source ZIP alongside the portable binaries.\n\n"
            + "".join(f"{source.name}\n{source.url}\nSHA-256: {source.sha256}\n\n" for source in upstream),
            encoding="utf-8")
        staged_binary = stage / binary_zip.name
        write_zip(staged_binary, [(f"{name}/{path.relative_to(stage).as_posix()}", path)
                                 for path in stage.rglob("*") if path.is_file() and path != staged_source])
        staged_source.replace(source_zip)
        staged_binary.replace(binary_zip)
    for package in (binary_zip, source_zip):
        print(f"{digest(package)}  {package}")


if __name__ == "__main__":
    main()

