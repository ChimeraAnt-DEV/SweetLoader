#!/usr/bin/env python3
"""Host tests for scripts/check_pinned_deps.py.

The checker guards two claims that are easy to get wrong and hard to notice: that every fetched
dependency is pinned to an immutable commit, and that NOTICE still describes what the build
actually fetches. A stale attribution is a licence problem, so the negative cases are the point
of these tests.
"""
from __future__ import annotations

import importlib.util
import pathlib
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent

SHA_A = "9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03"
SHA_B = "40626af88bd7df9a5fb80be7b25ac85b122d6c21"

CMAKE = """\
set(GLOSSHOOK_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/lib/ARM64/libGlossHook.a")
FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG {tag_a}
)
FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG {tag_b}
)
"""

NOTICE = """\
Bundled and fetched third-party components
------------------------------------------
  nlohmann/json       {sha_a}   MIT
  fmtlib/fmt          {sha_b}   MIT

Rust crates
-----------
  serde / serde_json              MIT OR Apache-2.0
"""

LOCK = """\
[[package]]
name = "serde"
version = "1.0.0"

[[package]]
name = "serde_json"
version = "1.0.0"
"""


def load():
    spec = importlib.util.spec_from_file_location(
        "check_pinned_deps", ROOT / "scripts" / "check_pinned_deps.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_tree(tmp, tag_a=SHA_A, tag_b=SHA_B, notice=None, lock=LOCK, gloss=True):
    tmp = pathlib.Path(tmp)
    (tmp / "rust").mkdir(parents=True, exist_ok=True)
    (tmp / "CMakeLists.txt").write_text(CMAKE.format(tag_a=tag_a, tag_b=tag_b), encoding="utf-8")
    if notice is None:
        notice = NOTICE.format(sha_a=SHA_A, sha_b=SHA_B)
    (tmp / "NOTICE").write_text(notice, encoding="utf-8")
    (tmp / "rust/Cargo.lock").write_text(lock, encoding="utf-8")
    if gloss:
        (tmp / "lib/ARM64").mkdir(parents=True, exist_ok=True)
        (tmp / "lib/ARM64/libGlossHook.a").write_bytes(b"archive")
    return tmp


def test_agreement_is_clean():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        assert module.check(make_tree(tmp)) == []


def test_branch_pin_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        problems = module.check(make_tree(tmp, tag_b="master"))
        assert any("not a full commit SHA" in p for p in problems), problems


def test_short_sha_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        problems = module.check(make_tree(tmp, tag_b=SHA_B[:10]))
        assert any("not a full commit SHA" in p for p in problems), problems


def test_unattributed_dependency_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        # fmt is fetched but has no NOTICE row. The template is formatted first, so the row to
        # drop is the one carrying the rendered commit rather than the {sha_b} placeholder.
        notice = NOTICE.format(sha_a=SHA_A, sha_b=SHA_B).replace(
            f"  fmtlib/fmt          {SHA_B}   MIT\n", "")
        problems = module.check(make_tree(tmp, notice=notice))
        assert any("fmtlib/fmt" in p and "not listed in NOTICE" in p for p in problems), problems


def test_stale_attribution_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        # NOTICE attributes a commit the build no longer fetches.
        problems = module.check(make_tree(tmp, tag_b="a" * 40))
        assert any("stale attribution" in p for p in problems), problems


def test_missing_vendored_archive_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        problems = module.check(make_tree(tmp, gloss=False))
        assert any("libGlossHook.a" in p for p in problems), problems


def test_missing_cargo_lock_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        root = make_tree(tmp)
        (root / "rust/Cargo.lock").unlink()
        problems = module.check(root)
        assert any("Cargo.lock" in p for p in problems), problems


def test_crate_absent_from_the_lock_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        notice = NOTICE.format(sha_a=SHA_A, sha_b=SHA_B) +             "  sha2                            MIT OR Apache-2.0\n"
        problems = module.check(make_tree(tmp, notice=notice))
        assert any("sha2" in p for p in problems), problems


def test_real_tree_is_clean():
    module = load()
    assert module.check(ROOT) == [], "the checked-in tree must satisfy its own checker"


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"PASS {test.__name__}")
        except Exception as exc:  # noqa: BLE001 - a test harness reports, it does not raise
            failed += 1
            print(f"FAIL {test.__name__}: {exc}")
    print(f"{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
