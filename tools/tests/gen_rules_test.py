#!/usr/bin/env python3
"""Host tests for tools/gen_rules.py.

The generator is what turns a Minecraft build into a signature rule, and it is the only thing
standing between a game update and a rule that detours the wrong address. Its whole premise is
that `.data.rel.ro` holds pre-relocation zeros offline, so the typeinfo and vtable pointers must
be rebuilt from `.rela.dyn` before anything is read. If that step regresses, every lookup quietly
returns nothing and the tool reports "NOT RESOLVED" for seams that are in fact present.

A real `libminecraftpe.so` is not needed to test that: the reader only uses the ELF header, the
section header table, `.rodata`, `.data.rel.ro`, `.text` and `.rela.dyn`. This builds a synthetic
ELF with the same shape as the stripped game binary and checks the derivation end to end.
"""
from __future__ import annotations

import importlib.util
import json
import pathlib
import struct
import sys
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent

RODATA_VADDR = 0x1000
DATARELRO_VADDR = 0x2000
TEXT_VADDR = 0x3000
TEXT_SIZE = 0x100
# Enough slots that a seam's index (the largest used is 40) is inside the table.
VTABLE_SLOTS = 64
R_AARCH64_RELATIVE = 1027


def load():
    spec = importlib.util.spec_from_file_location(
        "gen_rules", ROOT / "tools" / "gen_rules.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_elf(type_specs):
    """Build a stripped-binary-shaped ELF64 for the given RTTI types.

    type_specs is a list of (type_name, slot, slot_target) where slot_target is the address the
    vtable slot should hold, or None to leave it zero. The pointers are written as zeros and then
    supplied by R_AARCH64_RELATIVE relocations, exactly as the real binary is on disk.
    """
    rodata = b""
    str_addrs = {}
    for name, _, _ in type_specs:
        str_addrs[name] = RODATA_VADDR + len(rodata)
        rodata += name.encode() + b"\x00"

    words = []
    relocs = []

    def word_addr(index):
        return DATARELRO_VADDR + index * 8

    for name, slot, slot_target in type_specs:
        base = len(words)
        words += [0] * (4 + VTABLE_SLOTS)
        # The typeinfo struct: [0] its own vtable pointer, [1] the type-name string.
        relocs.append((word_addr(base + 1), str_addrs[name]))
        # The vtable: [base+2] offset-to-top (must be zero), [base+3] the typeinfo pointer,
        # [base+4..] the slots.
        relocs.append((word_addr(base + 3), word_addr(base)))
        if slot_target is not None:
            relocs.append((word_addr(base + 4 + slot), slot_target))

    datarelro = b"".join(struct.pack("<Q", w) for w in words)
    text = b"\x00" * TEXT_SIZE
    rela = b"".join(
        struct.pack("<QQq", vaddr, R_AARCH64_RELATIVE, addend) for vaddr, addend in relocs
    )

    shstr = b"\x00"
    name_offset = {}
    for section in (".rodata", ".data.rel.ro", ".text", ".rela.dyn", ".shstrtab"):
        name_offset[section] = len(shstr)
        shstr += section.encode() + b"\x00"

    # Lay the payloads out after the 64-byte ELF header, then the section header table.
    offset = 64
    placements = []
    for payload in (rodata, datarelro, text, rela, shstr):
        placements.append((offset, payload))
        offset += len(payload)
    offset += (offset % 8) and (8 - offset % 8)  # keep the shdr table 8-byte aligned
    shoff = offset

    (ro_off, _), (dr_off, _), (tx_off, _), (rela_off, _), (sh_off, _) = placements

    shdrs = [struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    shdrs.append(struct.pack("<IIQQQQIIQQ", name_offset[".rodata"], 1, 0, RODATA_VADDR,
                             ro_off, len(rodata), 0, 0, 8, 0))
    shdrs.append(struct.pack("<IIQQQQIIQQ", name_offset[".data.rel.ro"], 1, 0, DATARELRO_VADDR,
                             dr_off, len(datarelro), 0, 0, 8, 0))
    shdrs.append(struct.pack("<IIQQQQIIQQ", name_offset[".text"], 1, 0, TEXT_VADDR,
                             tx_off, len(text), 0, 0, 16, 0))
    shdrs.append(struct.pack("<IIQQQQIIQQ", name_offset[".rela.dyn"], 4, 0, 0,
                             rela_off, len(rela), 0, 0, 8, 24))
    shdrs.append(struct.pack("<IIQQQQIIQQ", name_offset[".shstrtab"], 3, 0, 0,
                             sh_off, len(shstr), 0, 0, 1, 0))

    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2  # ELFCLASS64
    header[5] = 1  # ELFDATA2LSB
    header[6] = 1  # EV_CURRENT
    struct.pack_into("<H", header, 0x10, 3)      # ET_DYN
    struct.pack_into("<H", header, 0x12, 0xB7)   # EM_AARCH64
    struct.pack_into("<I", header, 0x14, 1)
    struct.pack_into("<Q", header, 0x28, shoff)
    struct.pack_into("<H", header, 0x34, 64)
    struct.pack_into("<H", header, 0x3A, 64)     # e_shentsize
    struct.pack_into("<H", header, 0x3C, len(shdrs))
    struct.pack_into("<H", header, 0x3E, 5)      # e_shstrndx

    blob = bytearray(header)
    for _, payload in placements:
        blob += payload
    blob += b"\x00" * (shoff - len(blob))
    for shdr in shdrs:
        blob += shdr
    return bytes(blob)


def write_elf(elf_bytes, directory):
    path = pathlib.Path(directory) / "libminecraftpe.so"
    path.write_bytes(elf_bytes)
    return path


def elf_with_all_seams():
    """An ELF where every seam gen_rules knows about resolves to an executable slot."""
    return build_elf([
        ("18LivePlayerRenderer", 17, TEXT_VADDR + 0x10),
        ("14ClientInstance", 31, TEXT_VADDR + 0x20),
        ("20ClientNetworkHandler", 40, TEXT_VADDR + 0x30),
    ])


def test_relocations_rebuild_the_vtable_pointer():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        path = write_elf(elf_with_all_seams(), tmp)
        elf = module.Elf(str(path))

        # The name string lives in .rodata and is found by content.
        assert elf.find_str("18LivePlayerRenderer") == RODATA_VADDR, \
            "the first name sits at the base of .rodata"

        # The typeinfo struct is the word before the name pointer, and the vtable pointer is
        # only correct if the RELA addend was applied.
        ti = elf.typeinfo_struct("18LivePlayerRenderer")
        assert ti == DATARELRO_VADDR, "the typeinfo struct is the first word group"
        assert elf.read_u64(ti + 8) == RODATA_VADDR, "the name pointer came from the relocation"

        points = elf.vtable_address_points("18LivePlayerRenderer")
        assert len(points) == 1, f"expected exactly one primary vtable, got {points}"
        assert points[0][0] == ti, "the vtable's typeinfo pointer is the same struct"


def test_vtable_slot_resolves_through_the_relocation():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        path = write_elf(elf_with_all_seams(), tmp)
        elf = module.Elf(str(path))
        _, ap = elf.vtable_address_points("18LivePlayerRenderer")[0]
        slot = elf.read_u64(ap + 17 * 8)
        assert slot == TEXT_VADDR + 0x10, "slot 17 holds the relocated .text address"
        assert elf.is_executable(slot), "a .text address must read as executable"


def test_build_rule_emits_a_key_for_every_resolved_seam():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        path = write_elf(elf_with_all_seams(), tmp)
        rule = module.build_rule(module.Elf(str(path)), "1.26.60.28", "1.26.62.0")

        assert rule["min"] == "1.26.60.28"
        assert rule["max"] == "1.26.62.0"
        # Only the seams that carry a rule key contribute one.
        assert rule["playerRenderVtableIndex"] == 17
        assert rule["packetReadVtableIndex"] == 40
        assert "14ClientInstance slot 31" not in rule, "a probe-only seam sets no rule key"

        probes = rule["probes"]
        assert probes["playerRenderVtableIndex"]["resolved"] is True
        assert probes["playerRenderVtableIndex"]["address"] == hex(TEXT_VADDR + 0x10)
        assert probes["14ClientInstance slot 31"]["resolved"] is True
        assert probes["packetReadVtableIndex"]["resolved"] is True


def test_build_rule_refuses_a_slot_that_is_not_executable():
    module = load()
    # The RTTI is present but the slot points into .data.rel.ro, not .text. That is the shape of
    # a wrong slot index, and it must not be written into a rule.
    elf_bytes = build_elf([("18LivePlayerRenderer", 17, DATARELRO_VADDR)])
    with tempfile.TemporaryDirectory() as tmp:
        path = write_elf(elf_bytes, tmp)
        rule = module.build_rule(module.Elf(str(path)), "1.26.60.28", "1.26.60.28")

        assert "playerRenderVtableIndex" not in rule, \
            "a non-executable slot must not become a rule"
        probe = rule["probes"]["playerRenderVtableIndex"]
        assert probe["resolved"] is False
        assert "executable" in probe["reason"]


def test_build_rule_fails_closed_when_the_rtti_is_absent():
    module = load()
    # A build with none of the known type names: every seam is omitted, and the rule still has to
    # be valid JSON so the loader can read it and fall back.
    elf_bytes = build_elf([("99SomeOtherType", 3, TEXT_VADDR + 0x10)])
    with tempfile.TemporaryDirectory() as tmp:
        path = write_elf(elf_bytes, tmp)
        rule = module.build_rule(module.Elf(str(path)), "1.26.60.28", "1.26.60.28")

        assert "playerRenderVtableIndex" not in rule
        assert "packetReadVtableIndex" not in rule
        for label in ("playerRenderVtableIndex", "packetReadVtableIndex",
                      "14ClientInstance slot 31"):
            assert rule["probes"][label]["resolved"] is False, f"{label} should be unresolved"
        # The document has to survive a round trip: the loader parses it as JSON.
        assert json.loads(json.dumps(rule))["probes"]["playerRenderVtableIndex"]["resolved"] is False


def test_extract_so_pulls_the_arm64_library():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        apk = tmp / "minecraft.apk"
        payload = b"\x7fELF synthetic"
        with zipfile.ZipFile(apk, "w") as z:
            z.writestr("assets/foo.txt", "not the library")
            z.writestr("lib/arm64-v8a/libminecraftpe.so", payload)

        out = module.extract_so(str(apk), str(tmp / "libminecraftpe.so"))
        assert pathlib.Path(out).read_bytes() == payload


def test_extract_so_refuses_a_package_without_the_library():
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        apk = tmp / "minecraft.apk"
        with zipfile.ZipFile(apk, "w") as z:
            z.writestr("lib/armeabi-v7a/libminecraftpe.so", "wrong ABI")

        try:
            module.extract_so(str(apk), str(tmp / "out.so"))
        except SystemExit as exc:
            assert "libminecraftpe.so" in str(exc)
        else:
            raise AssertionError("a package with no arm64 library must be refused")


def test_fetch_apk_passes_a_local_path_through():
    module = load()
    # A local path is returned unchanged and must not be treated as a URL to download.
    assert module.fetch_apk("/tmp/some.apk", "/tmp/cache") == "/tmp/some.apk"


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"PASS {test.__name__}")
        except Exception as exc:  # noqa: BLE001 - a test harness reports, it does not raise
            failed += 1
            print(f"FAIL {test.__name__}: {exc}", file=sys.stderr)
    print(f"{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
