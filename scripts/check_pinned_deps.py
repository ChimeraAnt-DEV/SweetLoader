#!/usr/bin/env python3
"""Verify the fetched dependencies are pinned by commit and match NOTICE.

CMakeLists.txt fetches five repositories at configure time. Two rules matter:

  * Each must be pinned by a full commit SHA in GIT_TAG. A tag or a branch is a moving target:
    the same source tree would then fetch different code on different days, which defeats the
    reproducibility the rest of the build goes to some trouble to keep.
  * Every pinned dependency, and every crate NOTICE claims to ship, must actually appear where
    it is said to be. A dependency that is fetched but not attributed is a licence problem, and
    one attributed but no longer fetched is a stale claim.

The GlossHook archive is vendored rather than fetched, so it is checked for existence only.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

GIT_REPOSITORY = re.compile(r"GIT_REPOSITORY\s+(\S+)")
GIT_TAG = re.compile(r"GIT_TAG\s+(\S+)")
FULL_SHA = re.compile(r"^[0-9a-f]{40}$")
# The table rows are indented by two spaces, so the name cannot be anchored at column 0.
NOTICE_HEADER = re.compile(r"^\s*(\S+)\s+([0-9a-f]{40})\s+(\S+)\s*$", re.M)
# re.M: the rows are lines inside the block, so ^ has to match at a line start.
NOTICE_CRATE = re.compile(r"^\s{2}([A-Za-z0-9_-]+)\s+(?:/.*?)?\s+[A-Z]", re.M)
# The literal appears as set(GLOSSHOOK_PATH "..."), so it is not at the start of the line.
GLOSSHOOK = re.compile(r"GLOSSHOOK_PATH\s+\"([^\"]+)\"")
# `name = "x"` in Cargo.lock; a crate appears exactly once per resolved version.
LOCK_NAME = re.compile(r'^name = "([^"]+)"$', re.M)


def declared_pins(cmake: pathlib.Path) -> list[tuple[str, str]]:
    """Every (repository, tag) pair in the CMakeLists FetchContent declarations."""
    text = cmake.read_text(encoding="utf-8")
    repos = GIT_REPOSITORY.findall(text)
    tags = GIT_TAG.findall(text)
    if len(repos) != len(tags):
        raise SystemExit(
            f"{cmake}: {len(repos)} GIT_REPOSITORY entries but {len(tags)} GIT_TAG entries"
        )
    return list(zip(repos, tags))


def notice_pins(notice: pathlib.Path) -> list[tuple[str, str, str]]:
    """The (name, commit, spdx) rows of NOTICE's third-party table."""
    return NOTICE_HEADER.findall(notice.read_text(encoding="utf-8"))


def notice_crates(notice: pathlib.Path) -> set[str]:
    """The crate names NOTICE lists under "Rust crates"."""
    text = notice.read_text(encoding="utf-8")
    _, _, tail = text.partition("Rust crates")
    return {m.group(1) for m in NOTICE_CRATE.finditer(tail)}


def check(root: pathlib.Path) -> list[str]:
    problems: list[str] = []
    cmake = root / "CMakeLists.txt"
    notice = root / "NOTICE"

    pins = declared_pins(cmake)
    for repo, tag in pins:
        if not FULL_SHA.match(tag):
            problems.append(
                f"{repo} is pinned to {tag!r}, which is not a full commit SHA; a tag or a "
                f"branch can move under the build"
            )

    # Every fetched repository must be attributed in NOTICE.
    named = notice_pins(notice)
    notice_by_sha = {sha: name for name, sha, _ in named}
    for repo, tag in pins:
        if FULL_SHA.match(tag) and tag not in notice_by_sha:
            problems.append(f"{repo} pinned at {tag} is not listed in NOTICE")

    # Every commit NOTICE attributes must still be fetched by CMakeLists.
    declared_shas = {tag for _, tag in pins}
    for name, sha, _ in named:
        if sha not in declared_shas:
            problems.append(
                f"NOTICE attributes {name} at {sha}, but CMakeLists.txt does not fetch that "
                f"commit (stale attribution)"
            )

    # The vendored archive must be where CMakeLists says it is.
    match = GLOSSHOOK.search(cmake.read_text(encoding="utf-8"))
    if match is None:
        problems.append("CMakeLists.txt does not define GLOSSHOOK_PATH")
    else:
        # The path is relative to the CMake source dir, and the literal in CMakeLists is
        # ${CMAKE_CURRENT_SOURCE_DIR}/lib/ARM64/libGlossHook.a.
        rel = match.group(1).replace("${CMAKE_CURRENT_SOURCE_DIR}/", "")
        if not (root / rel).is_file():
            problems.append(f"the vendored archive CMakeLists links does not exist: {rel}")

    # Every crate NOTICE names must appear in Cargo.lock, and vice versa is not required (a
    # transitive crate need not be attributed).
    lock = root / "rust/Cargo.lock"
    if not lock.is_file():
        problems.append("rust/Cargo.lock is missing, so the crate set is not pinned")
    else:
        in_lock = set(LOCK_NAME.findall(lock.read_text(encoding="utf-8")))
        for crate in sorted(notice_crates(notice)):
            if crate not in in_lock:
                problems.append(
                    f"NOTICE lists the crate {crate}, but it is not in rust/Cargo.lock"
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
        print("pinned-deps: the pins and NOTICE disagree:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print("pinned-deps: every fetched dependency is pinned by commit and attributed in NOTICE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
