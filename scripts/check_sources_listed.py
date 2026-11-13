#!/usr/bin/env python3
"""Verify the build list, the source tree and the host test runner all agree.

Three things drift silently here, and each produces a build that passes while doing less than
it claims:

  1. A new .cpp that is never added to SWEETLOADER_SOURCES is not compiled into the library.
     Nothing fails; the symbol is simply absent at runtime.
  2. A new host test that is never registered in tests/cosmetics/run_tests.sh never runs, so a
     pure helper can rot untested while the suite still reports success.
  3. A source listed in CMakeLists.txt that no longer exists fails the configure with a
     confusing error instead of a precise one.

Every disagreement is reported in one run rather than stopping at the first.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

CMAKE_LIST = re.compile(r"set\(\s*SWEETLOADER_SOURCES(.*?)\n\s*\)", re.S)
CMAKE_ENTRY = re.compile(r"^\s*(src/[\w./-]+\.cpp)\s*$", re.M)
# Anchored to the start of the line: an unanchored match also hits the prose in a comment
# ("compile_run fixes those first"), which reads as a test named `fixes`.
COMPILE_RUN = re.compile(r"^compile_run\s+(\w+)", re.M)
TSAN_BANNER = re.compile(r'==\s+(\w+Test)\s+==')


def read_sources_listed(cmake: pathlib.Path) -> list[str]:
    text = cmake.read_text(encoding="utf-8")
    match = CMAKE_LIST.search(text)
    if match is None:
        raise SystemExit(f"{cmake}: no SWEETLOADER_SOURCES list found")
    return sorted(CMAKE_ENTRY.findall(match.group(1)))


def read_tests_registered(runner: pathlib.Path) -> set[str]:
    text = runner.read_text(encoding="utf-8")
    return set(COMPILE_RUN.findall(text)) | set(TSAN_BANNER.findall(text))


def check(root: pathlib.Path) -> list[str]:
    problems: list[str] = []

    cmake = root / "CMakeLists.txt"
    runner = root / "tests/cosmetics/run_tests.sh"

    listed = read_sources_listed(cmake)
    on_disk = sorted(
        p.relative_to(root).as_posix() for p in (root / "src").rglob("*.cpp")
    )

    for path in sorted(set(on_disk) - set(listed)):
        problems.append(
            f"{path} exists but is not in SWEETLOADER_SOURCES, so it is not compiled "
            f"into the library"
        )
    for path in sorted(set(listed) - set(on_disk)):
        problems.append(
            f"{path} is in SWEETLOADER_SOURCES but does not exist on disk"
        )

    registered = read_tests_registered(runner)
    test_files = sorted(
        p.stem for p in (root / "tests/cosmetics").glob("*Test.cpp")
    )

    for name in sorted(set(test_files) - registered):
        problems.append(
            f"tests/cosmetics/{name}.cpp exists but is never run by "
            f"tests/cosmetics/run_tests.sh"
        )
    for name in sorted(registered - set(test_files)):
        problems.append(
            f"run_tests.sh runs {name} but tests/cosmetics/{name}.cpp does not exist"
        )

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=str(pathlib.Path(__file__).resolve().parent.parent),
        help="the sweetloader checkout to check",
    )
    args = parser.parse_args()

    problems = check(pathlib.Path(args.root))
    if problems:
        print("sources-listed: the build list and the tree disagree:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print("sources-listed: sources, tests and the build list agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
