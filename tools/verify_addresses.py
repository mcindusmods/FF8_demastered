#!/usr/bin/env python3
"""Check DEMASTER's address tables against a real game DLL before anyone runs the patch.

Usage:
    python verify_addresses.py FFVIII_EFIGS.dll ../ff8_demaster/mymemory.h --table EFIGS
    python verify_addresses.py FFVIII_JP.dll    ../ff8_demaster/mymemory.h --table JP

For every address the patch writes to, this checks that the bytes there are what the patch code expects
(the instruction it overwrites, the operand it replaces, the import slot it calls through). It prints OK,
MISMATCH or INFO per entry, and the disassembly for anything that is not OK. Exit code 1 on any mismatch.
Only reads the DLL; needs capstone (pip install capstone).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from port_addresses import PeImage, parse_tables  # noqa: E402

import capstone  # noqa: E402

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = True


def imports_by_slot(image):
    """IAT slot RVA -> 'dll!function'."""
    data, slots = image.data, {}
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt = pe + 24
    import_rva = struct.unpack_from("<I", data, opt + 96 + 8)[0]
    offset = image.rva_to_offset(import_rva)
    while offset is not None:
        original_first, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, offset)
        if name_rva == 0:
            break
        dll = data[image.rva_to_offset(name_rva):].split(b"\0", 1)[0].decode()
        lookup = original_first or first_thunk
        k = 0
        while True:
            entry = image.u32(lookup + k * 4)
            if not entry:
                break
            if entry & 0x80000000:
                name = f"#{entry & 0xFFFF}"
            else:
                name = data[image.rva_to_offset(entry + 2):].split(b"\0", 1)[0].decode()
            slots[first_thunk + k * 4] = f"{dll}!{name}"
            k += 1
        offset += 20
    return slots


def disasm(image, rva, count=6):
    blob = image.read(rva, 64) or b""
    out = []
    for insn in MD.disasm(blob, image.image_base + rva):
        out.append(insn)
        if len(out) >= count:
            break
    return out


def show(image, rva, count=6):
    return "\n".join(f"      {i.address:08x}  {i.bytes.hex():<20} {i.mnemonic} {i.op_str}" for i in disasm(image, rva, count))


def length_of_instructions(image, rva, needed):
    """Bytes covered by whole instructions until at least `needed` bytes (must land on a boundary)."""
    total = 0
    for insn in disasm(image, rva, 32):
        total += insn.size
        if total >= needed:
            return total
    return total


def check(image, table, slots):
    results = []

    def add(name, status, note, rva=None, count=6):
        results.append((name, status, note, rva, count))

    def rva_of(name):
        return table.get(name)

    def target_of_rel(rva):
        b = image.read(rva, 5)
        if b and b[0] in (0xE8, 0xE9):
            return (rva + 5 + struct.unpack_from("<i", b, 1)[0]) & 0xFFFFFFFF
        return None

    def iat_call(rva):
        b = image.read(rva, 6)
        if b and b[:2] == b"\xff\x15":
            return struct.unpack_from("<I", b, 2)[0] - image.image_base
        return None

    # JMP-patched sites: InjectJMP(address, ..., size) overwrites `size` bytes; they must end on an instruction boundary
    jmp_sites = {
        "_BSPBACKADD1": 38, "_BSPBACKADD2": 9, "_BHPBACKADD1": 17, "_BHPBACKADD2": 5, "BCPBACKADD1": 18,
        "BCPBACKADD2": 5, "BCPBACKADD3": 5, "FCPBACKADD1": 18, "FCPBACKADD2": 6, "FIELD_CHAR_BLINK": 13,
        "_ASM_FIELDBGRETADDR3": 20, "_ASM_FIELDBGRETADDR2": 42, "CLTBACKADD1": 5, "CLTBACKADD2": 7,
        "WINDOWTITLE": 5, "BATTLEJMPPATCH1": 6, "IOFUNC1": 6, "IOFUNC2": 6, "IOFUNC5": 6,
    }
    for name, size in jmp_sites.items():
        rva = rva_of(name)
        if rva is None:
            continue
        covered = length_of_instructions(image, rva, size)
        add(name, "OK" if covered == size else "MISMATCH",
            f"overwrites {size} bytes, instructions there cover {covered}", rva, 8)

    # Specific expectations
    rva = rva_of("_BSPBACKADD1")
    if rva is not None:
        b = image.read(rva, 38)
        want = bytes.fromhex("ff7510894508680114000068e18000006a00565768588000006a0068e10d0000")
        ok = b[:len(want)] == want
        call = iat_call(rva + 32)
        add("_BSPBACKADD1 pattern", "OK" if ok else "MISMATCH",
            "push pixels; mov [ebp+8],eax; GL_UNSIGNED_BYTE, GL_BGRA, 0, ESI=height, EDI=width, GL_RGBA8, 0, GL_TEXTURE_2D",
            rva, 12)
        if call is not None and "DS_TEXIMG" in table:
            add("_BSPBACKADD1 call", "OK" if call == table["DS_TEXIMG"] else "MISMATCH",
                f"calls through {call:#x} ({slots.get(call, '?')}), DS_TEXIMG = {table['DS_TEXIMG']:#x}")
    rva = rva_of("_BSPBACKADD2")
    if rva is not None:
        call = iat_call(rva + 3)
        ok = image.read(rva, 3) == b"\xff\x75\x10" and call == table.get("DS_FREE")
        add("_BSPBACKADD2 pattern", "OK" if ok else "MISMATCH",
            f"push [ebp+10h]; call {slots.get(call, '?') if call else '?'}", rva)
    for name in ("DS_FREE", "DS_TEXIMG"):
        rva = rva_of(name)
        if rva is not None:
            add(name, "OK" if rva in slots else "MISMATCH", f"import slot: {slots.get(rva, 'not an import slot')}")
    rva = rva_of("CLTBACKADD1")
    if rva is not None:
        target = target_of_rel(rva)
        ok = target is not None and target == rva_of("UPLOADVRAM")
        add("CLTBACKADD1 call", "OK" if ok else "MISMATCH",
            f"call {target:#x}" if target is not None else "no CALL", rva)
    rva = rva_of("CLTBACKADD2")
    if rva is not None:
        ok = image.read(rva, 7) == bytes.fromhex("83babc00000000")
        add("CLTBACKADD2 pattern", "OK" if ok else "MISMATCH", "cmp dword ptr [edx+0BCh], 0", rva)
    rva = rva_of("FILTERPATCH2")
    if rva is not None:
        b = image.read(rva - 5, 10)
        ok = b[0] == 0x68 and struct.unpack_from("<I", b, 1)[0] == 0x2601 and b[5] == 0x68 \
            and struct.unpack_from("<I", b, 6)[0] == 0x2801
        add("FILTERPATCH2", "OK" if ok else "MISMATCH",
            "push GL_TEXTURE_MIN_FILTER (known DEMASTER quirk: the name, not the value; see FILTERPATCH2_VALUE)", rva - 5, 4)
    for i in (1, 3, 4):
        rva = rva_of(f"FILTERPATCH{i}")
        if rva is not None:
            b = image.read(rva, 10)
            ok = b[0] == 0x68 and struct.unpack_from("<I", b, 1)[0] == 0x2601 \
                and b[5] == 0x68 and struct.unpack_from("<I", b, 6)[0] in (0x2800, 0x2801)
            add(f"FILTERPATCH{i}", "OK" if ok else "MISMATCH",
                "push GL_LINEAR as the value, followed by push GL_TEXTURE_MAG/MIN_FILTER", rva - 5, 4)
    rva = rva_of("FILTERPATCH2_VALUE")
    if rva is not None:
        b = image.read(rva, 10)
        ok = b[0] == 0x68 and struct.unpack_from("<I", b, 1)[0] == 0x2601 and b[5] == 0x68 \
            and struct.unpack_from("<I", b, 6)[0] == 0x2801
        add("FILTERPATCH2_VALUE", "OK" if ok else "MISMATCH", "push GL_LINEAR; push GL_TEXTURE_MIN_FILTER", rva, 4)
    rva = rva_of("_BSPBACKADD1")
    if rva is not None:
        after, skip = image.read(rva + 38, 8), image.read(rva + 38 + 0x2C, 3)
        ok = after == bytes.fromhex("8b45fc83f8017e24") and skip == bytes.fromhex("8b7508")
        add("upscale skip (KEEP_HD_RESOLUTION)", "OK" if ok else "MISMATCH",
            "mov eax,[ebp-4]; cmp eax,1; jle -> mov esi,[ebp+8]", rva + 38, 4)
    rva = rva_of("UVFUNCPNTR")
    if rva is not None:
        ok = image.read(rva, 3) == b"\x80\x46\x08"
        add("UVFUNCPNTR", "OK" if ok else "MISMATCH", "add byte ptr [esi+8], imm8", rva, 2)
    for i in range(12):
        rva = rva_of(f"UVPATCH{i}")
        if rva is not None:
            first = disasm(image, rva, 1)
            ok = bool(first) and first[0].size == 4 and first[0].bytes[0] == 0x80
            add(f"UVPATCH{i}", "OK" if ok else "MISMATCH",
                f"4-byte imm8 instruction ({first[0].mnemonic} {first[0].op_str})" if first else "no instruction", rva, 2)
    rva = rva_of("IOFUNC3")
    if rva is not None:
        add("IOFUNC3", "OK" if image.read(rva, 3) == b"\xff\x75\x10" else "MISMATCH", "push [ebp+10h] (fseek)", rva, 3)
    rva = rva_of("IOFUNC5")
    if rva is not None:
        add("IOFUNC5", "OK" if image.read(rva, 6) == bytes.fromhex("8bc68b4c2450") else "MISMATCH",
            "mov eax, esi; mov ecx, [esp+50h] (re-executed by directIO_fopenReroute3)", rva, 3)
    rva = rva_of("IOFUNC6")
    if rva is not None:
        b = image.read(rva, 2)
        add("IOFUNC6", "OK" if 0x70 <= b[0] <= 0x7F else "MISMATCH", "short conditional jump (NOP'd)", rva, 2)
    for name in ("FIELDCHARENT1", "FIELDCHARENT2", "DISABLETPAGELIMIT"):
        rva = rva_of(name)
        if rva is not None:
            b = image.read(rva, 2)
            add(name, "OK" if 0x70 <= b[0] <= 0x7F else "MISMATCH", "short conditional jump (made JMP)", rva, 2)
    rva = rva_of("BGRESPATCH1")
    if rva is not None:
        for at in (rva, rva + 0xC):
            b = image.read(at - 1, 2)
            ok = b[0] in (0xC1, 0xD1, 0xD3) and (b[1] & 0xF8) == 0xE8
            add(f"BGRESPATCH1{'+0xC' if at != rva else ''}", "OK" if ok else "MISMATCH",
                "shr reg (ModRM byte rewritten to shl)", at - 1, 2)
    rva = rva_of("BTLMON4")
    if rva is not None:
        b = image.read(rva, 6)
        add("BTLMON4", "OK" if b[0] == 0x0F and 0x80 <= b[1] <= 0x8F else "MISMATCH", "6-byte jcc (made JMP)", rva, 2)
    for name in ("BTLMON0", "BTLMON1", "BTLMON2", "BTLMON3", "BTLMON6"):
        rva = rva_of(name)
        if rva is not None:
            add(name, "OK" if rva in image.relocations else "MISMATCH", "relocated pointer operand (tex provider)", rva - 2, 2)
    rva = rva_of("CHANGEMODE")
    if rva is not None:
        add("CHANGEMODE+0x1FC", "OK" if image.read(rva + 0x1FC, 1) == b"\xe8" else "MISMATCH", "call GetModeName", rva + 0x1FC, 2)
    rva = rva_of("SET_RESOLUTION")
    if rva is not None:
        b = image.read(rva + 0xE3, 8)
        ok = b[:4] == bytes.fromhex("ff760468")
        add("SET_RESOLUTION+0xE6", "OK" if ok else "MISMATCH",
            f"push [esi+4]; push {struct.unpack_from('<I', b, 4)[0]:#x} (re-pushed by AsmSetResolution)", rva + 0xE3, 3)
    sub, ext = rva_of("SUB_116059B0"), rva_of("LOAD_BATTLE_EXT")
    if sub is not None and ext is not None:
        jp = table.get("__language") == "JP"
        shifts = (0x130F + 6, 0x1320 + 6) if jp else (0x13ED + 6, 0x13FE + 6)
        v1, v2 = image.u32(sub + shifts[0]), image.u32(sub + shifts[1])
        h = struct.unpack("<H", image.read(ext + 0xBC1 + 1, 2))[0]
        ok = v1 == 0x300 and v2 == 0x300 and h == 0x180
        add("monster atlas operands", "OK" if ok else "MISMATCH",
            f"SUB_116059B0+{shifts[0]:#x}={v1:#x}, +{shifts[1]:#x}={v2:#x}, LOAD_BATTLE_EXT+0xBC2={h:#x} (want 0x300 0x300 0x180)",
            sub + shifts[0] - 6, 3)
    rva = rva_of("WMSET014_PATH_PUSH")
    if rva is not None:
        b = image.read(rva, 5)
        target = struct.unpack_from("<I", b, 1)[0] - image.image_base if b[0] == 0x68 else None
        text = image.read(target, 34) if target is not None else None
        ok = text == b"FIELD.FS\\field_hd_new\\wmset_014_0\0"
        add("WMSET014_PATH_PUSH", "OK" if ok else "MISMATCH", "push \"FIELD.FS\\field_hd_new\\wmset_014_0\"", rva, 2)
    # WORLD_VEHICLES: the chara.one read call, LoadImage's entry (hooked with MinHook) and the wmset guest address
    for call, target, what in (("WM_CHARA_READ_CALL", "WM_READ_FILE", "chara.one read"),):
        rva, want = rva_of(call), rva_of(target)
        if rva is None or want is None:
            continue
        got = target_of_rel(rva)
        add(call, "OK" if got == want and image.read(rva, 1) == b"\xe8" else "MISMATCH",
            f"call {target} ({what})" + ("" if got == want else f", calls {got:#x}" if got is not None else ", not a call"), rva, 2)
        # the hook reads the emulated CPU state from [esp+4]: the last host stack change before the call must be
        # "push esi" or "push edi" (the recompiler keeps the state pointer in one of them)
        last = None
        for back in range(1, 0x100):
            insns = list(MD.disasm(image.read(rva - back, back) or b"", image.image_base + rva - back))
            if not insns or sum(i.size for i in insns) != back:
                continue
            stack = [i for i in insns if i.mnemonic in ("push", "pop", "call", "pushfd", "popfd", "pushal", "popal")
                     or (i.mnemonic in ("add", "sub") and i.op_str.startswith("esp"))]
            if stack:
                last = stack[-1]
                break
        ok = last is not None and last.mnemonic == "push" and last.op_str in ("esi", "edi")
        add(call + " state", "OK" if ok else "MISMATCH",
            f"last stack change before the call: {last.mnemonic + ' ' + last.op_str if last else 'none found'}",
            (last.address - image.image_base) if last else rva, 2)
    rva = rva_of("GUEST_LOADIMAGE")
    if rva is not None:
        # recompiled LoadImage(RECT*, data): state in esi, reads [guest esp + 4] (the RECT), and the upload-TIM and chara
        # upload functions both call it twice / once (the bytes DEMASTER checks before hooking)
        start = image.read(rva, 15)
        ok = start == bytes.fromhex("558bec568b75088b462c83c00450e8")
        callers = 0
        for vaddr, blob in image.exec_blobs():
            k = blob.find(b"\xe8")
            while k >= 0:
                if k + 5 <= len(blob) and (vaddr + k + 5 + struct.unpack_from("<i", blob, k + 1)[0]) & 0xFFFFFFFF == rva:
                    callers += 1
                k = blob.find(b"\xe8", k + 1)
        add("GUEST_LOADIMAGE", "OK" if ok and callers >= 10 else "MISMATCH",
            f"recompiled LoadImage entry, expected prologue {'found' if ok else 'NOT found'}, called from {callers} place(s)", rva, 6)
    rva = rva_of("FIELD_READ_FILE")
    if rva is not None:
        # FIELD_LAYERS: recompiled sm_pc_read(filename, buffer), hooked with MinHook (prologue checked before hooking);
        # read_field_data calls it 16 times (17 in the JP game), so it is called from many places
        start = image.read(rva, 15)
        ok = start == bytes.fromhex("558bec568b7508568146 2cfcfeffff".replace(" ", ""))
        callers = 0
        for vaddr, blob in image.exec_blobs():
            k = blob.find(b"\xe8")
            while k >= 0:
                if k + 5 <= len(blob) and (vaddr + k + 5 + struct.unpack_from("<i", blob, k + 1)[0]) & 0xFFFFFFFF == rva:
                    callers += 1
                k = blob.find(b"\xe8", k + 1)
        add("FIELD_READ_FILE", "OK" if ok and callers >= 16 else "MISMATCH",
            f"recompiled sm_pc_read entry, expected prologue {'found' if ok else 'NOT found'}, called from {callers} place(s)", rva, 6)
    rva = rva_of("BGRESPATCH1")
    if rva is not None:
        # FIELD_BG_RESOLUTION 6/8: mov eax,[logical w] / shr / mov [w],eax / mov eax,[logical h] / shr / push / mov [h],eax
        # (texturepatch_v2_fieldLayers.cpp checks the same bytes)
        b = lambda off, n=1: image.read(rva + off, n)
        ok = (b(-0xC) == b"\xa1" and b(-1, 2) == b"\xd1\xe8" and b(1) == b"\xa3" and b(6) == b"\xa1" and b(0xB, 2) == b"\xd1\xe8"
              and b(0x12) == b"\xa3")
        add("BGRESPATCH1 layout", "OK" if ok else "MISMATCH",
            "field background buffer size code (FIELD_BG_RESOLUTION rewrites the two loads)", rva - 0xC, 8)
    value = table.get("WMSET_GUEST")
    if value is not None:
        # guest address: one function computes all 48 section pointers as value + [value + 4 * i]
        lea = b"\x8d\x04\x85" + struct.pack("<I", value)
        uses = 0
        for vaddr, blob in image.exec_blobs():
            k = blob.find(lea)
            while k >= 0:
                if blob.find(b"\x05" + struct.pack("<I", value), k + 7, k + 24) >= 0:
                    uses += 1
                k = blob.find(lea, k + 1)
        add("WMSET_GUEST", "OK" if uses == 48 else "MISMATCH",
            f"guest address {value:#x} (not an RVA): section pointer = base + [base + 4*i] for {uses} of 48 sections")
    value = table.get("PSX_VRAM")
    if value is not None:
        # guest address: read_vram_palette (guest 0x467370) and its neighbour compute a CLUT address as
        # ((clut & 0x7fc0) + (clut & 0x3f)) << 5 + base, i.e. and eax, 0x7fc0 ... shl eax, 5 / add eax, base
        tail = b"\xc1\xe0\x05\x05" + struct.pack("<I", value)
        clut_reads = refs = 0
        for vaddr, blob in image.exec_blobs():
            k = blob.find(tail)
            while k >= 0:
                if blob.find(b"\x25\xc0\x7f\x00\x00", max(0, k - 0x40), k) >= 0:
                    clut_reads += 1
                k = blob.find(tail, k + 1)
            k = blob.find(struct.pack("<I", value))
            while k >= 0:
                refs += 1
                k = blob.find(struct.pack("<I", value), k + 1)
        add("PSX_VRAM", "OK" if clut_reads >= 2 and refs >= 10 else "MISMATCH",
            f"guest address {value:#x} (not an RVA): CLUT address computed from it in {clut_reads} place(s), {refs} reference(s)")
    rv, wp, sb = rva_of("READ_VRAM_PALETTE"), rva_of("WRITE_PALETTE"), rva_of("GUEST_STACK_BASE")
    if rv is not None and wp is not None and sb is not None:
        # FIELD_LAYERS palette slots: recompiled read_vram_palette / write_palette_to_driver, both starting with
        # push ebp / mov ebp, esp / mov eax, [guest stack base], called only from write_palette_texture_set
        stack = b"\x55\x8b\xec\xa1" + struct.pack("<I", image.image_base + sb)
        def callers(target):
            found = []
            for vaddr, blob in image.exec_blobs():
                k = blob.find(b"\xe8")
                while k >= 0:
                    if k + 5 <= len(blob) and (vaddr + k + 5 + struct.unpack_from("<i", blob, k + 1)[0]) & 0xFFFFFFFF == target:
                        found.append(vaddr + k)
                    k = blob.find(b"\xe8", k + 1)
            return found
        rv_ok = image.read(rv, 8) == stack and image.read(rv + 8, 7) == bytes.fromhex("83ec08568b7508")
        wp_ok = image.read(wp, 8) == stack and image.read(wp + 8, 8) == bytes.fromhex("568b7508568b562c")
        body = image.read(wp, 0x180)
        wp_ok = wp_ok and b"\x05\x98\x00\x00\x00" in body and b"\x05\xa0\x00\x00\x00" in body  # texture_set->tex_header, ->palette
        rc, wc = callers(rv), callers(wp)
        together = len(rc) == 1 and len(wc) >= 1 and all(abs(c - rc[0]) < 0x400 for c in wc)
        add("READ_VRAM_PALETTE", "OK" if rv_ok and together else "MISMATCH",
            f"recompiled read_vram_palette, prologue {'found' if rv_ok else 'NOT found'}, {len(rc)} caller(s)", rv, 6)
        add("WRITE_PALETTE", "OK" if wp_ok and together else "MISMATCH",
            f"recompiled write_palette_to_driver, prologue and texture_set offsets {'found' if wp_ok else 'NOT found'}, "
            f"{len(wc)} caller(s) {'next to' if together else 'NOT next to'} read_vram_palette's", wp, 6)
    for name in ("GUEST_STACK_BASE", "GUEST_PAGE_TABLE"):
        rva = rva_of(name)
        if rva is not None:
            refs = sum(1 for r in image.relocations if image.is_exec(r) and image.u32(r) == image.image_base + rva)
            add(name, "OK" if refs > 100 else "MISMATCH", f"data, referenced by {refs} instruction(s) (recompiled code uses it everywhere)")
    rva = rva_of("SKIP_SLASHSCREEN")
    if rva is not None:
        current = image.read(rva, 1)
        add("SKIP_SLASHSCREEN", "INFO", f"data byte, {'initially ' + current.hex() if current else 'zero-initialised'}"
            " (DEMASTER writes 01)")
    for name in ("WORLDGETTEXIDX0", "WORLDGETTEXIDX1", "WORLDGETTEXIDX2", "WORLDGETTEXIDX3",
                 "BGFILENAME1", "BGFILENAME2", "CHECKTEXREPAVAIL", "STEAMADDR"):
        rva = rva_of(name)
        if rva is not None:
            refs = sum(1 for r in image.relocations if image.is_exec(r) and image.u32(r) == image.image_base + rva)
            near = sum(1 for r in image.relocations if image.is_exec(r)
                       and 0 < abs((image.u32(r) or 0) - (image.image_base + rva)) <= 0x40)
            add(name, "OK" if refs else "INFO",
                f"data, referenced by {refs} instruction(s)" + ("" if refs else f", {near} reference(s) within 0x40"))
    for name in ("UPLOADVRAM", "BHP", "BHP_ATLAS", "NEWGLTEX_CHARA", "SUB_116059B0", "LOAD_BATTLE_EXT",
                 "SET_RESOLUTION", "MORE_DEBUG_LOG", "NULLSUB_DEBUG", "BATTLEJMPPATCH2"):
        rva = rva_of(name)
        if rva is not None:
            callers = 0
            for vaddr, blob in image.exec_blobs():
                start = 0
                while True:
                    k = blob.find(b"\xe8", start)
                    if k < 0 or k + 5 > len(blob):
                        break
                    if (vaddr + k + 5 + struct.unpack_from("<i", blob, k + 1)[0]) & 0xFFFFFFFF == rva:
                        callers += 1
                    start = k + 1
            add(name, "INFO", f"called from {callers} place(s)", rva, 3)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dll")
    parser.add_argument("mymemory_h")
    parser.add_argument("--table", choices=("EFIGS", "JP"), required=True)
    parser.add_argument("--column", type=int, default=3)
    args = parser.parse_args()
    image = PeImage(args.dll)
    efigs, jp = parse_tables(args.mymemory_h, args.column)
    table = dict(efigs if args.table == "EFIGS" else jp)
    table["__language"] = args.table
    slots = imports_by_slot(image)
    results = check(image, table, slots)
    mismatches = 0
    for name, status, note, rva, count in results:
        print(f"{status:9} {name:28} {note}")
        if status == "MISMATCH":
            mismatches += 1
            if rva is not None:
                print(show(image, rva, count))
    print(f"\n{len(results)} checks, {mismatches} mismatch(es)")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
