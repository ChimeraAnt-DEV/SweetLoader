#!/usr/bin/env python3
"""Host tests for scripts/check_sources_listed.py.

The checker exists because two kinds of drift fail silently: a new .cpp that never reaches
SWEETLOADER_SOURCES is simply absent from the library, and a new host test that is never
registered never runs. A checker that misses either is worse than none, so both are exercised
here against a synthetic tree.
"""
from __future__ import annotations

import importlib.util
import pathlib
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent

CMAKE_TEMPLATE = """\
cmake_minimum_required(VERSION 3.22)
set(SWEETLOADER_SOURCES
        src/pl/Mod.cpp
        {extra}
)
"""

RUNNER_TEMPLATE = """\
#!/usr/bin/env bash
compile_run ModTest \\
  "$here/ModTest.cpp"
{extra}
"""


def load():
    spec = importlib.util.spec_from_file_location(
        "check_sources_listed", ROOT / "scripts" / "check_sources_listed.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_tree(tmp, sources, tests, extra_cmake="", extra_runner=""):
    """Lay out a minimal tree the checker can read."""
    tmp = pathlib.Path(tmp)
    (tmp / "src/pl").mkdir(parents=True)
    (tmp / "tests/cosmetics").mkdir(parents=True)
    for source in sources:
        path = tmp / source
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// synthetic\n", encoding="utf-8")
    for test in tests:
        (tmp / f"tests/cosmetics/{test}.cpp").write_text("// synthetic\n", encoding="utf-8")
    listed = "\n        ".join(sources)
    (tmp / "CMakeLists.txt").write_text(
        CMAKE_TEMPLATE.format(extra=f"{listed}\n        {extra_cmake}".strip()),
        encoding="utf-8")
    registered = "".join(f"compile_run {t} \\\n  \"$here/{t}.cpp\"\n" for t in tests)
    (tmp / "tests/cosmetics/run_tests.sh").write_text(
        RUNNER_TEMPLATE.format(extra=registered + extra_runner), encoding="utf-8")
    return tmp


def test_agreement_is_clean():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        root = make_tree(tmp, ["src/pl/Mod.cpp"], ["ModTest"])
        assert module.check(root) == []


def test_unlisted_source_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        # The file is on disk but not in SWEETLOADER_SOURCES.
        root = make_tree(tmp, ["src/pl/Mod.cpp"], ["ModTest"])
        (root / "src/pl/Forgotten.cpp").write_text("// new\n", encoding="utf-8")
        problems = module.check(root)
        assert any("Forgotten.cpp" in p and "not compiled" in p for p in problems), problems


def test_missing_source_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        # Listed in CMakeLists but deleted: the configure would fail confusingly.
        root = make_tree(tmp, ["src/pl/Mod.cpp"], ["ModTest"])
        (root / "src/pl/Mod.cpp").unlink()
        problems = module.check(root)
        assert any("does not exist on disk" in p for p in problems), problems


def test_unregistered_test_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        root = make_tree(tmp, ["src/pl/Mod.cpp"], ["ModTest"])
        (root / "tests/cosmetics/NewTest.cpp").write_text("// new\n", encoding="utf-8")
        problems = module.check(root)
        assert any("NewTest.cpp" in p and "never run" in p for p in problems), problems


def test_registered_test_without_a_file_is_reported():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        root = make_tree(tmp, ["src/pl/Mod.cpp"], ["ModTest"])
        (root / "tests/cosmetics/ModTest.cpp").unlink()
        problems = module.check(root)
        assert any("does not exist" in p for p in problems), problems


def test_prose_mentioning_compile_run_is_not_a_test():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        # A comment that says "compile_run fixes those first" must not read as a test named
        # `fixes`; an unanchored regex does exactly that and reports a phantom failure.
        root = make_tree(tmp, ["src/pl/Mod.cpp"], ["ModTest"],
                         extra_runner="# compile_run fixes those first\n")
        assert module.check(root) == [], "a comment is not a registration"


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
