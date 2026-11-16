#!/usr/bin/env python3
"""Port DEMASTER patch addresses from one build of the game DLL to another (EFIGS -> JP, or 1.0.3.0 -> a new patch).

Usage:
    python port_addresses.py FFVIII_EFIGS.dll FFVIII_JP.dll ../ff8_demaster/mymemory.h

How it works
  * Code addresses: the bytes around the source address become a pattern. Bytes that legitimately change between
    builds are masked: relocated absolute addresses (from the .reloc table) and relative CALL/JMP/Jcc operands
    (via capstone when installed, otherwise by opcode heuristics). The pattern grows until it matches exactly one
    place in the target DLL.
  * Data addresses (globals, import slots): every instruction in the source that references the address through a
    relocated operand is ported as code, and the operand at the matching place in the target is read back.
    The answers are voted on.
  * Offsets DEMASTER applies on top of table entries (WINDOWTITLE+0x12, SUB_116059B0+0x13ED+6, ...) are ported as
    well, so you can see whether the JP offsets in the source code are right.

Output: one line per address with a status, then C++ lines for mymemory.h.
Status: SAME (agrees with the existing target table), NEW (target table has nothing), DIFFERENT, AMBIGUOUS,
NOT FOUND. Only trust SAME/NEW/DIFFERENT results that report a unique match.
"""
import argparse
import re
import struct
import sys
from collections import Counter

try:
    import capstone  # optional, pip install capstone
except ImportError:  # pragma: no cover
    capstone = None


class PeImage:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError(f"{path}: not a PE file")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: bad PE signature")
        sections, optional_size = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x10B:
            raise ValueError(f"{path}: only 32-bit images are supported")
        self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        reloc_rva, reloc_size = struct.unpack_from("<II", d, opt + 96 + 5 * 8)
        self.sections = []
        for i in range(sections):
            s = opt + optional_size + i * 40
            name = d[s:s + 8].rstrip(b"\0").decode("latin-1")
            vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", d, s + 8)
            characteristics = struct.unpack_from("<I", d, s + 36)[0]
            self.sections.append((name, vaddr, max(vsize, rsize), rptr, rsize, bool(characteristics & 0x20000000)))
        self.relocations = set()
        offset = self.rva_to_offset(reloc_rva) if reloc_size else None
        end = offset + reloc_size if offset is not None else 0
        while offset is not None and offset + 8 <= end:
            page, block = struct.unpack_from("<II", d, offset)
            if block < 8:
                break
            for k in range((block - 8) // 2):
                entry = struct.unpack_from("<H", d, offset + 8 + k * 2)[0]
                if entry >> 12 == 3:  # IMAGE_REL_BASED_HIGHLOW
                    self.relocations.add(page + (entry & 0xFFF))
            offset += block
        self.exec_ranges = [(v, v + min(sz, rs)) for (_, v, sz, _, rs, x) in self.sections if x]

    def rva_to_offset(self, rva):
        for (_, vaddr, size, rptr, rsize, _) in self.sections:
            if vaddr <= rva < vaddr + rsize:
                return rptr + rva - vaddr
        return None

    def is_exec(self, rva):
        return any(a <= rva < b for a, b in self.exec_ranges)

    def read(self, rva, length):
        offset = self.rva_to_offset(rva)
        if offset is None:
            return None
        return self.data[offset:offset + length]

    def u32(self, rva):
        b = self.read(rva, 4)
        return struct.unpack("<I", b)[0] if b and len(b) == 4 else None

    def exec_blobs(self):
        for (_, vaddr, size, rptr, rsize, x) in self.sections:
            if x:
                yield vaddr, self.data[rptr:rptr + rsize]


def relative_operand_mask(image, start, length):
    """Byte positions (relative to start) holding relative branch displacements."""
    blob = image.read(start, length)
    masked = set()
    if blob is None:
        return masked
    if capstone is not None:
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
        # x86 re-synchronises after a few bytes, so a window starting mid-instruction is fine
        for insn in md.disasm(blob, start):
            if insn.group(capstone.CS_GRP_JUMP) or insn.group(capstone.CS_GRP_CALL):
                encoding = insn.encoding
                if encoding.imm_size and any(op.type == capstone.x86.X86_OP_IMM for op in insn.operands):
                    rel = insn.address - start + encoding.imm_offset
                    masked.update(range(rel, rel + encoding.imm_size))
        return masked
    for i in range(len(blob)):  # heuristic fallback
        b = blob[i]
        if b in (0xE8, 0xE9):
            masked.update(range(i + 1, i + 5))
        elif b == 0x0F and i + 1 < len(blob) and 0x80 <= blob[i + 1] <= 0x8F:
            masked.update(range(i + 2, i + 6))
    return masked


def make_pattern(image, start, length):
    blob = image.read(start, length)
    if blob is None or len(blob) < length:
        return None
    mask = [True] * length
    for i in range(length):
        for k in range(4):
            if (start + i - k) in image.relocations and i - k >= -3:
                mask[i] = False
    for rel in relative_operand_mask(image, start, length):
        if 0 <= rel < length:
            mask[rel] = False
    return blob, mask


def to_regex(blob, mask):
    parts = []
    for b, m in zip(blob, mask):
        parts.append(re.escape(bytes([b])) if m else b".")
    return re.compile(b"".join(parts), re.DOTALL)


def search(target, blob, mask, limit=3):
    regex = to_regex(blob, mask)
    found = []
    for vaddr, data in target.exec_blobs():
        for m in regex.finditer(data):
            found.append(vaddr + m.start())
            if len(found) >= limit:
                return found
    return found


WINDOWS = [(0, 24), (0, 32), (8, 32), (16, 48), (0, 64), (32, 64), (48, 96), (0, 128), (64, 128), (96, 192)]


def _window_hits(source, target, rva, pre, post):
    pattern = make_pattern(source, rva - pre, pre + post)
    if pattern is None:
        return None
    blob, mask = pattern
    if sum(mask) < 12:
        return None
    return [h + pre for h in search(target, blob, mask)]


def port_code(source, target, rva):
    """Returns (target_rva or None, note).

    The first window with exactly one match wins, but only if no larger window points somewhere else: short windows
    over repetitive code (several identical stores to neighbouring globals) can be unique and still wrong.
    """
    best_ambiguous = None
    for index, (pre, post) in enumerate(WINDOWS):
        hits = _window_hits(source, target, rva, pre, post)
        if not hits:
            continue
        if len(hits) > 1:
            best_ambiguous = hits
            continue
        candidate, confirmations = hits[0], 0
        for larger_pre, larger_post in WINDOWS[index + 1:]:
            larger = _window_hits(source, target, rva, larger_pre, larger_post)
            if not larger:
                continue
            if candidate not in larger:
                return None, f"conflict: -{pre:#x}/+{post:#x} gives {candidate:#x}, -{larger_pre:#x}/+{larger_post:#x} gives " \
                             + ", ".join(f"{h:#x}" for h in larger)
            if len(larger) == 1:
                confirmations += 1
                if confirmations >= 2:
                    break
        note = f"unique, window -{pre:#x}/+{post:#x}" + ("" if confirmations else ", not confirmed by a larger window")
        return candidate, note
    if best_ambiguous:
        return None, "ambiguous: " + ", ".join(f"{h:#x}" for h in best_ambiguous)
    return None, "no match"


def port_data(source, target, rva, max_refs=12):
    source_va = source.image_base + rva
    votes, notes = Counter(), []
    refs = [r for r in sorted(source.relocations) if source.is_exec(r) and source.u32(r) == source_va]
    for ref in refs[:max_refs]:
        site, note = port_code(source, target, ref)
        if site is None:
            continue
        value = target.u32(site)
        if value is None:
            continue
        votes[value - target.image_base] += 1
    if not votes:
        return None, f"no portable reference ({len(refs)} found)"
    (best, count), total = votes.most_common(1)[0], sum(votes.values())
    if count * 2 <= total:
        return None, "references disagree: " + ", ".join(f"{k:#x}x{v}" for k, v in votes.most_common(4))
    return best, f"{count}/{total} references agree"


def port(source, target, rva):
    if source.is_exec(rva):
        return port_code(source, target, rva)
    return port_data(source, target, rva)


def parse_tables(header_path, column):
    text = open(header_path, encoding="utf-8", errors="replace").read()
    tables = {}
    for name in ("PATCHADDRESS_EFIGS", "PATCHADDRESS_JP"):
        m = re.search(name + r"\s*=\s*\{(.*?)\n\};", text, re.S)
        entries = {}
        if m:
            body = re.sub(r"//[^\n]*", "", m.group(1))  # ignore commented-out entries
            for key, values in re.findall(r"\{\s*(\w+)\s*,\s*std::vector<unsigned int>\s*\{([^}]*)\}", body):
                numbers = [int(v.strip(), 16) for v in values.split(",") if v.strip()]
                if len(numbers) > column and numbers[column]:
                    entries[key] = numbers[column]
        tables[name] = entries
    return tables["PATCHADDRESS_EFIGS"], tables["PATCHADDRESS_JP"]


# Offsets DEMASTER's code adds to table entries
DERIVED = [
    ("WINDOWTITLE", 0x11), ("WINDOWTITLE", -0xA3), ("WINDOWTITLE", 0x162), ("WINDOWTITLE", -0x5F),
    ("WINDOWTITLE", -0xAB), ("CHANGEMODE", 0x1FC), ("SET_RESOLUTION", 0xE6),
    ("SUB_116059B0", 0x13ED + 6), ("SUB_116059B0", 0x13FE + 6), ("LOAD_BATTLE_EXT", 0xBC1 + 1),
    ("_ASM_FIELDBGRETADDR2", 0x78), ("_ASM_FIELDBGRETADDR2", 0x99), ("BGRESPATCH1", 0xC),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source_dll")
    parser.add_argument("target_dll")
    parser.add_argument("mymemory_h", nargs="?")
    parser.add_argument("--column", type=int, default=3, help="game version column in mymemory.h (3 = 1.0.3.0)")
    parser.add_argument("--rva", action="append", default=[], help="extra NAME=0xRVA to port")
    args = parser.parse_args()

    source, target = PeImage(args.source_dll), PeImage(args.target_dll)
    print(f"source {args.source_dll}: base {source.image_base:#x}, {len(source.relocations)} relocations")
    print(f"target {args.target_dll}: base {target.image_base:#x}, {len(target.relocations)} relocations")
    if capstone is None:
        print("capstone not installed: using opcode heuristics for branch masking (pip install capstone)")

    efigs, jp = parse_tables(args.mymemory_h, args.column) if args.mymemory_h else ({}, {})
    for extra in args.rva:
        name, value = extra.split("=", 1)
        efigs[name] = int(value, 16)
    if not efigs:
        parser.error("nothing to port: pass mymemory.h or --rva NAME=0xRVA")

    results, lines = {}, []
    print(f"\n{'NAME':24} {'SOURCE':>10} {'TARGET(old)':>11} {'PORTED':>10}  STATUS / NOTE")
    for name in sorted(efigs, key=lambda n: efigs[n]):
        rva = efigs[name]
        ported, note = port(source, target, rva)
        results[name] = ported
        old = jp.get(name)
        if ported is None:
            status = "AMBIGUOUS" if note.startswith("ambiguous") else "NOT FOUND"
        elif old is None:
            status = "NEW"
        else:
            status = "SAME" if old == ported else "DIFFERENT"
        print(f"{name:24} {rva:#10x} {(f'{old:#x}' if old else '-'):>11} {(f'{ported:#x}' if ported else '-'):>10}  {status}  ({note})")
        if ported is not None and status in ("NEW", "DIFFERENT"):
            lines.append(f"{{ {name:<20}, std::vector<unsigned int>{{ 0x0, 0x0, 0x0, {ported:#09x} }} }},")

    print("\nOffsets used in code on top of table entries:")
    for base, offset in DERIVED:
        if base not in efigs:
            continue
        rva = efigs[base] + offset
        ported, note = port(source, target, rva)
        target_base = results.get(base) or jp.get(base)
        relative = f"{ported - target_base:+#x}" if ported is not None and target_base else "?"
        print(f"  {base}{offset:+#x}: source {rva:#x} -> target {(f'{ported:#x}' if ported else '-')}"
              f" = {base}{relative} in target  ({note})")

    if lines:
        print("\nLines for the target table in mymemory.h (review before use):")
        print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
