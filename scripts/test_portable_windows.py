#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise a portable ZIP after extraction to a path with spaces."""
import argparse
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
from zipfile import ZipFile

from dependency_sources import digest

TESTS = {
    "dingobench_verification_test": ["applicationEcho"],
    "dingobench_storage_test": [],
    "dingobench_receive_test": [],
    "dingobench_worklist_test": ["nativeQueriesAndLiveWorkspace", "managedServerLifecycle"],
    "dingobench_profiles_test": [],
    "dingobench_logging_test": [],
    "dingobench_ui_test": [],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zip", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--qt", required=True, type=Path)
    parser.add_argument("--tools", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--native", action="store_true", help="Also test the Windows desktop platform plugin")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    system = Path(os.environ["SystemRoot"])
    env = {name: value for name, value in os.environ.items()
           if not name.upper().startswith(("QT_", "QML", "DCM", "DINGOBENCH_"))}
    env["PATH"] = os.pathsep.join(str(path) for path in (system / "System32", system))
    with tempfile.TemporaryDirectory(prefix="DingoBench portable test ", dir=output) as directory:
        with ZipFile(args.zip) as archive:
            paths = [PurePosixPath(entry) for entry in archive.namelist()]
            if len({path.parts[0] for path in paths}) != 1 or any(
                    path.is_absolute() or ".." in path.parts or "\\" in str(path) for path in paths):
                raise ValueError("Unsafe archive layout.")
            allowed_exes = {"DingoBench.exe", "DingoBenchReceiver.exe", "wlmscpfs.exe"}
            if {p.name for p in paths if p.suffix == ".exe"} != allowed_exes or any(
                    p.suffix in {".pdb", ".obj", ".lib", ".dcm", ".wl", ".log"} for p in paths):
                raise ValueError("Unexpected executable or development data in the portable archive.")
            archive.extractall(directory)
        stage = Path(directory) / paths[0].parts[0]
        licenses = stage / "licenses"
        assert "GNU GENERAL PUBLIC LICENSE" in (stage / "LICENSE").read_text(encoding="utf-8")
        assert "OFFIS" in (licenses / "DCMTK-COPYRIGHT.txt").read_text(encoding="utf-8")
        assert (licenses / "MaterialSymbols/NOTICE.txt").is_file()
        assert len(list((licenses / "Qt").rglob("LGPL-3.0-only.txt"))) == 2
        assert (stage / "platforms/qwindows.dll").is_file()
        assert (stage / "platforms/qoffscreen.dll").is_file()
        assert (stage / "vcruntime140.dll").is_file()
        source_note = (stage / "SOURCE.txt").read_text(encoding="utf-8")
        source_zip = args.zip.with_name(args.zip.stem + "-source.zip")
        assert digest(source_zip) in source_note
        app = stage / "DingoBench.exe"
        for platform in (("offscreen", "windows") if args.native else ("offscreen",)):
            env["QT_QPA_PLATFORM"] = platform
            with (output / f"startup-{platform}.txt").open("wb") as log:
                subprocess.run([app, "--smoke-test"], cwd=output, env=env, timeout=15,
                               stdout=log, stderr=subprocess.STDOUT, check=True)
            print(f"PASS: portable {platform} startup and shutdown", flush=True)
        utility = stage / "wlmscpfs.exe"
        version = subprocess.check_output([utility, "--version"], cwd=output, env=env, timeout=10)
        assert b"3.7.1" in version, version

        # Add regression drivers only after testing the untouched extraction.
        shutil.copy2(args.qt / "bin/Qt6Test.dll", stage / "Qt6Test.dll")
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["DINGOBENCH_TEST_APP"] = str(app)
        env["DINGOBENCH_WLMSCPFS"] = str(utility)
        for tool in ("storescu", "storescp", "findscu"):
            env[f"DINGOBENCH_{tool.upper()}"] = str(args.tools.resolve() / f"{tool}.exe")
        for name, functions in TESTS.items():
            driver = stage / f"{name}.exe"
            shutil.copy2(args.build / driver.name, driver)
            result = output / f"{name}.txt"
            subprocess.run([driver, *functions, "-o", f"{result},txt"], cwd=output,
                           env=env, timeout=90, check=True)
            contents = result.read_text(encoding="utf-8")
            totals = next(line for line in contents.splitlines() if line.startswith("Totals: "))
            if "0 failed, 0 skipped" not in totals:
                raise RuntimeError(f"{name}: {totals}")
            print(f"PASS: {name}: {totals}", flush=True)


if __name__ == "__main__":
    main()

