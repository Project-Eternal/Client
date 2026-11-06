#!/usr/bin/env python3
"""Prepare the executable of the client (3.3.5a, build 12340) for the SRW static recompiler.

The client has its base relocations stripped and mixes data into .text, so this recovers what SRW needs:

  relocations.csv          `<va of dword>,<value of dword>,` per absolute address (external relocations)
  data_areas.sci           data blocks embedded in .text (jump tables etc.): kept as data, never decoded as code
  fixup_interpret_as_code.sci  code addresses taken inside .text (callbacks, function pointers): SRW follows fixups from
                           data to code by itself but not these
  instruction_replacements.sci  currently empty (cpuid is translated by SRW and answers from the port's feature mask: MMX, SSE and SSE2
                           with the `sse` setting on, which is the default, none with it off; 3DNow! and CMOV are never reported, so
                           those paths are not taken. An instruction SRW cannot translate calls WoW_Unsupported_c, which stops the program)
  patched exe              copy of the exe in which every `fs:[0]` access (the SEH chain head) addresses an
                           ordinary dword (the SafeSEH table slot in .sxdata) instead, so SRW never sees an
                           fs: prefix; the 0x64 prefix byte becomes a nop, so no instruction moves

usage: prepare_exe.py WoW.exe relocations.csv patched.exe data_areas.sci instruction_replacements.sci fixup_interpret_as_code.sci
Needs capstone + pefile (pip install capstone pefile).
"""
import re
import bisect
import os
import sys

# tables of pairs (data pointer, function pointer) are recognised; PREPARE_WIDE=0 turns that off
WIDE = os.environ.get("PREPARE_WIDE", "1") == "1"
import collections
import pefile
import capstone
from capstone import x86

SEH_HEAD_OFFSET = 0  # first dword of .sxdata: the SafeSEH handler table, unused by a recompiled program
PADDING = (0x90, 0xcc)
# opcodes whose 32-bit immediate is an absolute address (everything else with an in-image immediate is a number)
IMM_MNEMONICS = ("push", "mov", "cmp")
# the start of a typical MSVC function, for unreferenced code found while classifying gaps
# replacement text for instructions that SRW cannot translate (none needed at the moment; verbatim text must manage
# proc/endp itself, so prefer teaching SRW the instruction: cpuid and rdtsc are native in SRW)
REPLACE_TEXT = {}
X87_ALIASES = {(0xdc, 0xd0), (0xdc, 0xd8), (0xdd, 0xc8), (0xde, 0xd0), (0xdf, 0xd0), (0xdf, 0xd8), (0xd8, 0xd0), (0xd8, 0xd8)}
PROLOGUES = (b"\x55\x8b\xec", b"\x6a\xff\x68", b"\x8b\xff\x55", b"\x53\x56\x57", b"\x83\xec", b"\x81\xec", b"\x56\x8b",
             b"\x51\x56", b"\x8b\x44\x24", b"\x8b\x4c\x24", b"\x33\xc0\xc3", b"\x55\x8b", b"\x53\x8b", b"\x8b\x54\x24")


# Roots that came from a weak pointer (a data dword that looks like a code address because of a neighbour that does), from a
# data pointer or from an immediate (a number such as the flag 0x800000 in the middle of a function), and whose chain of instructions stopped a stricter root from being accepted later (it overlapped): the analysis is
# run again without them.
BANNED_WEAK = set()
WEAK_CONFLICTS = set()


def main(*args):
    for iteration in range(10):
        WEAK_CONFLICTS.clear()
        run(*args)
        new = WEAK_CONFLICTS - BANNED_WEAK
        if not new:
            break
        print("weak or data roots that blocked real code, banned for another run: %s" % ", ".join("%x" % v for v in sorted(new)), file=sys.stderr)
        BANNED_WEAK.update(new)


def run(exe, out_relocs, out_exe, out_ignored, out_replacements, out_code_fixups):
    pe = pefile.PE(exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    # resources are not emitted by SRW, so nothing may point there (three false positives otherwise)
    end = base + max(x.VirtualAddress + x.Misc_VirtualSize for x in pe.sections if not x.Name.startswith(b".rsrc"))
    entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    sections = {}
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode()
        sections[name] = (base + s.VirtualAddress, s.Misc_VirtualSize, s.get_data()[:min(s.SizeOfRawData, s.Misc_VirtualSize)])
    tva, tsize, text = sections[".text"]
    tend = tva + tsize

    def aspect_of_window(va):
        """the aspect from the window's client size. 0x86a1a0 (cdecl, a RECT to fill) gives the size the OS layer keeps, or
        GetClientRect; 4:3 without one. Replaces 0x50f7c0, which reads the CVars widescreen and gxResolution."""
        call = 0x86a1a0 - (va + 11)
        return (bytes([0x83, 0xec, 0x10,                           # sub esp, 16 (a RECT)
                       0x89, 0xe0,                                 # mov eax, esp
                       0x50,                                       # push eax
                       0xe8]) + (call & 0xffffffff).to_bytes(4, "little") +   # call 0x86a1a0
                bytes([0x83, 0xc4, 0x04,                           # add esp, 4
                       0x85, 0xc0,                                 # test eax, eax
                       0x74, 0x14,                                 # je 4:3
                       0x8b, 0x44, 0x24, 0x0c,                     # mov eax, [esp + 12] (bottom)
                       0x85, 0xc0,                                 # test eax, eax
                       0x7e, 0x0c,                                 # jle 4:3
                       0xdb, 0x44, 0x24, 0x08,                     # fild dword [esp + 8] (right)
                       0xda, 0x74, 0x24, 0x0c,                     # fidiv dword [esp + 12]
                       0x83, 0xc4, 0x10,                           # add esp, 16
                       0xc3,                                       # ret
                       0xc7, 0x04, 0x24, 0x04, 0, 0, 0,            # 4:3: mov dword [esp], 4
                       0xc7, 0x44, 0x24, 0x04, 0x03, 0, 0, 0,      # mov dword [esp + 4], 3
                       0xdb, 0x04, 0x24,                           # fild dword [esp]
                       0xda, 0x74, 0x24, 0x04,                     # fidiv dword [esp + 4]
                       0x83, 0xc4, 0x10,                           # add esp, 16
                       0xc3]))                                     # ret

    # WardenClient::Initialize (0x7da8c0) loads a signed module from the image and calls into it, which a static recompilation cannot
    # do. It returns at once (xor al, al; ret) and registers no handler for SMSG_WARDEN_DATA.
    # Loose files (Data\\... on disk before the archives): Storm's open (0x424780) strips the local-file scope bits unless the byte
    # [0xab7b7c] is 1; the startup (0x402b20) clears it (0x421750: mov byte [0xab7b7c], 0) and sets the default scope [0xab7b80]
    # (0x421760, argument 0). Immediates: 0x421756 -> 1 (allowed), 0x402b30 (push 0 before the call of 0x421760) -> 1 (every open
    # asks for the local file first).
    PATCHES = {0x7da8c0: bytes([0x32, 0xc0, 0xc3]), 0x50f7c0: aspect_of_window(0x50f7c0),
               0x421756: bytes([0x01]), 0x402b30: bytes([0x01]),
               # the signature check of the interface files: 0x8165e0(toc, 0, key, &md5) returns 3 when the MD5 of the listed files
               # matches the signature, and the client exits on 2 or 4. The comparison (0x8166ce) jumps to the result for equal
               # (0x816758), so edited interface files pass; the signature file itself is still needed.
               0x8166ce: bytes([0xe9, 0x85, 0x00, 0x00, 0x00])}
    is_client = tva <= 0x7da8c0 < tva + len(text) and bytes(text[0x7da8c0 - tva:0x7da8c0 - tva + 6]) == bytes.fromhex("b9601ad300e8")
    HANDLER_PATCHES = PATCHES if is_client else {}
    text = bytearray(text)
    sections[".text"] = (tva, tsize, text)
    for patch_va, patch in HANDLER_PATCHES.items():
        text[patch_va - tva:patch_va - tva + len(patch)] = patch

    def in_image(v):
        return base + 0x1000 <= v < end

    def in_text(v):
        return tva <= v < tend

    def textlike_dwords(data):
        """which aligned dwords of a section look like ASCII text (up to three characters followed by NULs, or four)"""
        def printable(b):
            return 0x20 <= b < 0x7f

        flags = []
        for o in range(0, len(data) - 3, 4):
            b = data[o:o + 4]
            n = 0
            while n < 4 and printable(b[n]):
                n += 1
            v = int.from_bytes(b, "little")
            # a 4-aligned code address (an entry of a table of functions) is not text even if its bytes are printable
            flags.append((n == 4 or (n >= 1 and not any(b[n:]))) and not (in_text(v) and v & 3 == 0))
        return flags

    def in_text_run(flags, index):
        """a dword within a run of text is a piece of a string, not a pointer"""
        lo, hi = max(0, index - 2), min(len(flags), index + 3)
        return sum(flags[lo:hi]) >= 4

    # areas that hold RVAs / metadata rather than absolute addresses
    skip = []
    for idx in (0, 1, 2, 5, 6, 12, 13):
        d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[idx]
        if d.VirtualAddress:
            skip.append((base + d.VirtualAddress, base + d.VirtualAddress + d.Size))
    for entry_ in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        n = len(entry_.imports) + 1
        for thunk in (entry_.struct.OriginalFirstThunk, entry_.struct.FirstThunk):
            if thunk:
                skip.append((base + thunk, base + thunk + 4 * n))

    def skipped(va):
        return any(a <= va < b for a, b in skip)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    # ---- data pointers into .text (vtables, jump tables, callbacks) ----
    def text_pointer_dwords(data, va0):
        """yield (va, value) for aligned dwords pointing into .text"""
        for o in range(0, len(data) - 3, 4):
            v = int.from_bytes(data[o:o + 4], "little")
            if in_text(v) and not skipped(va0 + o):
                yield va0 + o, v

    def near_pointer(ptrs, data, va0, va, step):
        """a text pointer next to va in the given direction, possibly across up to 3 empty (zero) dwords"""
        for k in range(1, 5):
            n = va + step * k
            if n in ptrs:
                return True
            o = n - va0
            if o < 0 or o + 4 > len(data):
                return False
            if int.from_bytes(data[o:o + 4], "little") != 0:
                return False
        return False

    def pair_table_interior(ptrs, data, va0, va):
        """a member of a table of pairs (a pointer to data, then a pointer to a function): the next or the previous code pointer
        is two dwords away, and the dwords in between point into the image. The first and the last pair of a table have such a
        neighbour on one side only."""
        def data_pointer(n):
            o = n - va0
            if o < 0 or o + 4 > len(data):
                return False
            v = int.from_bytes(data[o:o + 4], "little")
            return in_image(v) and not in_text(v)

        return (va - 8 in ptrs and data_pointer(va - 4) and data_pointer(va + 4)) or (
            va + 8 in ptrs and data_pointer(va + 4) and data_pointer(va - 4)) or (
            va - 8 in ptrs and data_pointer(va - 4)) or (va + 8 in ptrs and data_pointer(va + 4))

    roots = {entry}
    root_kind = {entry: 'entry'}   # how a root was found (for the overlap report)
    chain_root = {}                # decoded va -> the root whose descent decoded it
    cur_kind = ['?']
    data_ptrs = {}   # va of pointer dword -> value, accepted pointers in non-.text sections
    weak = {}        # pointers that only have a pointing neighbour going for them (switch tables), validated later
    for name, (va0, vsize, data) in sections.items():
        if name in (".text", ".rsrc", ".sxdata"):
            continue
        tl = textlike_dwords(data)
        ptrs = {va: v for va, v in text_pointer_dwords(data, va0) if not in_text_run(tl, (va - va0) // 4)}
        for va, v in ptrs.items():
            prev = text[v - 1 - tva] if v > tva else 0x90
            if v & 3 == 0 and prev in PADDING:
                data_ptrs[va] = v
                roots.add(v)
                root_kind.setdefault(v, 'data')
            elif any(near_pointer(ptrs, data, va0, va, step) for step in (-4, 4)) or (WIDE and v % 16 == 0 and pair_table_interior(ptrs, data, va0, va)):
                weak[va] = v

    # ---- recursive descent over .text ----
    covered = bytearray(tsize)  # 1 = byte belongs to a decoded instruction
    insn_starts = set()
    undecodable = collections.Counter()
    mnemonics = collections.Counter()  # of the instructions kept, for checking SRW supports them all
    replacements = {}                  # va -> (size, text)
    branch_sources = collections.defaultdict(list)  # target -> [(source va, mnemonic)]
    imm_candidates = set()             # immediates that may be code addresses, validated with try_root
    sizes = {}                         # va -> instruction size
    code_relocs = {}
    fs_sites = []
    image = bytearray(open(exe, "rb").read())
    for patch_va, patch in HANDLER_PATCHES.items():
        patch_offset = pe.get_offset_from_rva(patch_va - base)
        image[patch_offset:patch_offset + len(patch)] = patch
    if ".sxdata" in sections:
        seh_head = sections[".sxdata"][0] + SEH_HEAD_OFFSET
        assert SEH_HEAD_OFFSET + 4 <= sections[".sxdata"][1]
    else:
        # 3.3.5 has no .sxdata: the last dword of .zdata (a 4 KB section of zeros nothing points into) takes its place
        seh_head = sections[".zdata"][0] + sections[".zdata"][1] - 4

    # Data pointers that look like function starts (aligned, behind padding) are tried after the code reachable from the entry
    # point is decoded, and only accepted if what they decode does not collide with it: a 0xcc inside the displacement of an
    # instruction (`mov [esi + 0xcc], edx`) makes a dword that points behind it look like one.
    deferred_roots = sorted(roots - {entry})
    work = [entry]
    seen_roots = {entry}

    # Tables of code pointers kept in .text (switch tables: >= 3 aligned dwords in a row pointing into the text near them) are
    # data: the code in front of them can fall into them (behind a call that never returns, or a jump table of a switch) and
    # decoded as instructions they swallow the function that follows.
    barrier = bytearray(tsize)
    o = 0
    while o + 4 <= tsize:
        k = o
        while k + 4 <= tsize:
            v = int.from_bytes(text[k:k + 4], "little")
            if in_text(v) and abs(v - (tva + k)) < 0x20000:
                k += 4
            else:
                break
        # a `ret` and the padding that aligns the table (c3 8d 49 00 = lea ecx, [ecx]) read as a pointer: the table begins behind it
        while k - o >= 12 and text[o] == 0xc3:
            o += 4
        if (k - o) >= 12 and (tva + o) & 3 == 0:
            for q in range(o, k):
                barrier[q] = 1
            o = k
        else:
            o += 4

    def push(t):
        if in_text(t) and t not in seen_roots:
            seen_roots.add(t)
            root_kind.setdefault(t, 'branch')
            work.append(t)
            return True
        return False

    def descend(strict=False):
        """decode everything reachable from the work list; in strict mode any conflict (invalid opcode, overlap of
        an already decoded instruction) undoes everything decoded since the call and returns False"""
        undo_starts, undo_relocs, undo_fs, undo_roots = [], [], [], []
        ok = True
        while work and ok:
            va = work.pop()
            root = va
            prev_barrier = True   # a chain that starts in a barrier is code reached by a jump (a false table: 6a 00 6a 00 ...)
            while in_text(va):
                if va in insn_starts:
                    break
                off = va - tva
                if barrier[off] and not prev_barrier:
                    break   # falls into a table
                insns = list(md.disasm(text[off:off + 16], va, 1))
                if not insns and text[off] in (0xd8, 0xdc, 0xdd, 0xde, 0xdf) and 0xc0 <= text[off + 1] <= 0xff and (
                        (text[off], text[off + 1] & 0xf8) in X87_ALIASES):
                    # undocumented 2-byte x87 aliases (fcom2, fcomp3, fxch4, fcomp5, fstp8/9) that capstone rejects
                    if strict and any(covered[off:off + 2]):
                        ok = False
                        break
                    insn_starts.add(va)
                    undo_starts.append((va, 2))
                    covered[off] = covered[off + 1] = 1
                    va += 2
                    continue
                if not insns or (strict and any(covered[off:off + insns[0].size])):
                    if strict and os.environ.get("PREPARE_TRACE_FAIL") and int(os.environ["PREPARE_TRACE_FAIL"], 16) <= va < int(os.environ["PREPARE_TRACE_FAIL"], 16) + int(os.environ.get("PREPARE_TRACE_RANGE", "0x40"), 16):
                        print("strict descent stops at %x: %s, covered %s" % (va, insns and insns[0].mnemonic, list(covered[off:off + 8])), file=sys.stderr)
                    if not insns and not strict:
                        undecodable[bytes(text[off:off + 3])] += 1
                    if strict and insns:
                        # whose instruction is in the way? if it came from a weak pointer, that root is to blame
                        for c in range(off, off + insns[0].size):
                            if not covered[c]:
                                continue
                            for back in range(16):
                                start = tva + c - back
                                if start in insn_starts and start + sizes.get(start, 1) > tva + c:
                                    if os.environ.get("PREPARE_TRACE_FAIL"):
                                        r = chain_root.get(start)
                                        print("  in the way: %x from root %s (%s)" % (start, r is not None and hex(r), root_kind.get(r)), file=sys.stderr)
                                    if root_kind.get(chain_root.get(start)) in ('weak', 'data', 'imm'):
                                        WEAK_CONFLICTS.add(chain_root[start])
                                    break
                            break
                    ok = not strict
                    break
                insn = insns[0]
                insn_starts.add(va)
                chain_root[va] = root
                prev_barrier = bool(barrier[off])
                mnemonics[insn.mnemonic] += 1
                sizes[va] = insn.size
                undo_starts.append((va, insn.size))
                if insn.mnemonic in REPLACE_TEXT:
                    replacements[va] = (insn.size, REPLACE_TEXT[insn.mnemonic])
                else:
                    replacements.pop(va, None)
                for k in range(insn.size):
                    covered[off + k] = 1
                enc = insn.encoding
                mn = insn.mnemonic

                def add_reloc(at, v):
                    if at not in code_relocs:
                        undo_relocs.append(at)
                    code_relocs[at] = v

                if insn.prefix[1] == 0x64 and enc.disp_size == 4 and insn.disp == 0 and not any(
                        op.type == x86.X86_OP_MEM and (op.mem.base or op.mem.index) for op in insn.operands):
                    fs_sites.append((va, enc.disp_offset))
                    undo_fs.append(1)
                    add_reloc(va + enc.disp_offset, seh_head)
                elif enc.disp_size == 4 and in_image(insn.disp & 0xffffffff):
                    add_reloc(va + enc.disp_offset, insn.disp & 0xffffffff)  # a displacement is data or a table
                is_branch = insn.group(capstone.CS_GRP_JUMP) or insn.group(capstone.CS_GRP_CALL)
                if enc.imm_size == 4 and mn in IMM_MNEMONICS and not is_branch:
                    for op in insn.operands:
                        if op.type == x86.X86_OP_IMM and in_image(op.imm & 0xffffffff):
                            add_reloc(va + enc.imm_offset, op.imm & 0xffffffff)
                            if in_text(op.imm & 0xffffffff):
                                imm_candidates.add(op.imm & 0xffffffff)  # a number in that range is not a pointer
                if is_branch and insn.operands and insn.operands[0].type == x86.X86_OP_IMM:
                    t = insn.operands[0].imm & 0xffffffff
                    if strict and not in_text(t):
                        if os.environ.get("PREPARE_TRACE_FAIL"):
                            print("strict descent: branch at %x leaves the text (%x)" % (va, t), file=sys.stderr)
                        ok = False
                        break
                    branch_sources[t].append((va, mn))
                    if push(t):
                        undo_roots.append(t)
                if insn.group(capstone.CS_GRP_RET) or mn in ("jmp", "ud2", "hlt"):
                    break
                va += insn.size
            else:
                if strict:
                    ok = False  # ran off the end of .text
        if not ok:
            for va, size in undo_starts:
                insn_starts.discard(va)
                replacements.pop(va, None)
                for k in range(size):
                    covered[va - tva + k] = 0
            for at in undo_relocs:
                code_relocs.pop(at, None)
            for _ in undo_fs:
                fs_sites.pop()
            for t in undo_roots:
                seen_roots.discard(t)
            work.clear()
        return ok

    descend()

    # ---- gaps: unreferenced code or data embedded in .text ----
    def gaps():
        i = 0
        while i < tsize:
            if covered[i]:
                i += 1
                continue
            j = i
            while j < tsize and not covered[j]:
                j += 1
            yield i, j
            i = j

    # the base of a jump table (jmp [reg*4 + base] whose first entry is a code address) is data: a weak, data or immediate root there
    # decodes the table as instructions and swallows the case addresses (0x8080c0 in 3.3.5: cases 12-21 of the spell switch at 0x807f10 were never
    # reached, "jump to the bad address 808021" when the player cast a spell)
    jt_bases = set()
    for m in re.finditer(rb'\xff\x24[\x85\x8d\x95\x9d\xb5\xbd\xad](....)', bytes(text), re.S):
        table = int.from_bytes(m.group(1), "little")
        o = table - tva
        if 0 <= o <= len(text) - 4:
            first = int.from_bytes(text[o:o + 4], "little")
            if in_text(first) and abs(first - table) < 0x20000:
                jt_bases.add(table)

    def try_root(t):
        """decode from t provisionally; keep it only if the result is clean and consistent with existing code"""
        if t in insn_starts:
            return True
        if t in jt_bases and cur_kind[0] != 'table':
            return False
        if not in_text(t) or covered[t - tva] or t in seen_roots:
            return False
        seen_roots.add(t)
        root_kind.setdefault(t, cur_kind[0])
        work.append(t)
        if os.environ.get("PREPARE_TRACE_ROOT") and t == int(os.environ["PREPARE_TRACE_ROOT"], 16):
            import traceback
            traceback.print_stack(limit=4, file=sys.stderr)
        accepted = descend(strict=True)
        if os.environ.get("PREPARE_TRACE_ROOT") and t == int(os.environ["PREPARE_TRACE_ROOT"], 16):
            print("root %x accepted: %s" % (t, accepted), file=sys.stderr)
        if accepted:
            return True
        seen_roots.discard(t)
        return False

    cur_kind[0] = 'data'
    for v in deferred_roots:
        # a data dword that points into an instruction (0x790500: the 0xcc of `mov [esi + 0xcc], edx`) is accepted when that
        # function has not been decoded yet; once a later root needs the function, the dword is banned and the analysis repeated
        if v not in BANNED_WEAK:
            try_root(v)

    def resolve_candidates():
        """immediates that look like code addresses become roots only if decoding from them is clean and consistent"""
        cur_kind[0] = 'imm'
        while imm_candidates:
            t = imm_candidates.pop()
            if t not in BANNED_WEAK:
                try_root(t)

    def table_roots():
        """runs of >= 2 dword code pointers inside gaps (jump tables kept in .text) are code roots"""
        cur_kind[0] = 'table'
        found = False
        # jmp [reg*4 + table]: the first entry is a case even when the table has just that one (a switch whose byte index table
        # maps everything to case 0: 0x8e3ff5 in 3.3.5, the table is followed by the byte indices, no run of two)
        for m in re.finditer(rb'\xff\x24[\x85\x8d\x95\x9d\xb5\xbd\xad](....)', bytes(text), re.S):
            t = int.from_bytes(m.group(1), "little")
            o = t - tva
            if 0 <= o <= len(text) - 4 and not covered[o]:
                v = int.from_bytes(text[o:o + 4], "little")
                if in_text(v) and abs(v - t) < 0x20000:
                    found |= bool(try_root(v))
        for i, j in list(gaps()):
            # a table after byte indices need not be aligned: try every alignment
            for r in range(4):
                run = []
                for o in range(i + (r - tva - i) % 4, j - 3, 4):
                    v = int.from_bytes(text[o:o + 4], "little")
                    # the entries of a jump table are the cases of one function: near the table (this keeps the bytes of an
                    # instruction that has not been reached yet, such as a table address followed by push 0, from being a table)
                    if in_text(v) and abs(v - (tva + o)) < 0x20000:
                        run.append(v)
                        continue
                    if len(run) >= 2:
                        found |= any([try_root(t) for t in run])
                    run = []
                if len(run) >= 2:
                    found |= any([try_root(t) for t in run])
        return found

    def data_dword(va):
        for name, (va0, vsize, data) in sections.items():
            if name != ".text" and va0 <= va < va0 + len(data) - 3:
                return int.from_bytes(data[va - va0:va - va0 + 4], "little")
        return None

    def zero_gap_neighbour(va):
        """an accepted pointer next to the dword, possibly across up to 3 empty slots (a table of static initializers or
        terminators has holes: the pointer at 0x82e050 sat behind two null entries)"""
        for step in (-4, 4):
            for k in range(1, 5):
                n = va + step * k
                if n in data_ptrs:
                    return True
                if data_dword(n) != 0:
                    break
        return False

    def pair_neighbour(va):
        """an entry of a table of pairs (a pointer to data, then a pointer to code): the code pointer two dwords away, in either
        direction, is accepted, and the dwords on both sides of this one point into the image (data)"""
        def data_pointer(n):
            v = data_dword(n)
            return v is not None and in_image(v) and not in_text(v)

        return (va - 8 in data_ptrs or va + 8 in data_ptrs) and data_pointer(va - 4) and data_pointer(va + 4)

    def accept_weak():
        """switch tables in .rdata: a target is only believable if it directly follows code that is already decoded (or
        padding); it must then also decode cleanly. Address order lets consecutive case blocks chain."""
        cur_kind[0] = 'weak'
        found = False
        changed = True
        while changed:
            changed = False
            for va, v in sorted(weak.items(), key=lambda kv: kv[1]):
                prev = v - 1 - tva
                # next to an accepted pointer it is a table entry (static initializers): only decoding has to succeed
                between = zero_gap_neighbour(va) or (WIDE and v % 16 == 0 and pair_neighbour(va))
                if not (between or v in insn_starts or covered[prev] or text[prev] in PADDING):
                    continue
                del weak[va]
                if v in BANNED_WEAK:
                    continue
                if try_root(v):
                    if os.environ.get("PREPARE_DUMP_WEAK"):
                        print("weak accepted %x -> %x" % (va, v), file=sys.stderr)
                    data_ptrs[va] = v
                    found = changed = True
        weak.clear()
        return found

    def delay_load_slots():
        """the lazy import helper of the program keeps a slot per function in .data, initially pointing to a small thunk
        in .text; the thunk ends with `jmp [slot]`. Nothing else refers to the thunks, so they are found from the jump."""
        cur_kind[0] = 'delay'
        found = False
        o = text.find(b"\xff\x25")
        while o >= 0:
            slot = int.from_bytes(text[o + 2:o + 6], "little")
            if in_image(slot) and not in_text(slot) and slot % 4 == 0 and slot not in data_ptrs:
                for name, (va0, vsize, data) in sections.items():
                    if name != ".text" and va0 <= slot < va0 + len(data) - 3:
                        v = int.from_bytes(data[slot - va0:slot - va0 + 4], "little")
                        if in_text(v) and 0 < tva + o - v < 0x30 and try_root(v):
                            data_ptrs[slot] = v
                            found = True
                        break
            o = text.find(b"\xff\x25", o + 1)
        return found

    delay_load_slots()

    for _ in range(12):
        resolve_candidates()
        found = table_roots()
        for i, j in list(gaps()):
            a, b = i, j
            while a < b and text[a] in PADDING:
                a += 1
            while b > a and text[b - 1] in PADDING:
                b -= 1
            if a < b and (tva + a) & 3 == 0 and any(text[a:a + len(p)] == p for p in PROLOGUES):
                cur_kind[0] = 'prologue'
                found |= try_root(tva + a)
        if not found:
            break

    for _ in range(4):
        if not accept_weak():
            break
        for _ in range(12):
            resolve_candidates()
            found = table_roots()
            for i, j in list(gaps()):
                a, b = i, j
                while a < b and text[a] in PADDING:
                    a += 1
                while b > a and text[b - 1] in PADDING:
                    b -= 1
                if a < b and (tva + a) & 3 == 0 and any(text[a:a + len(p)] == p for p in PROLOGUES):
                    cur_kind[0] = 'prologue'
                    found |= try_root(tva + a)
            if not found:
                break

    ignored = []
    gap_relocs = {}
    for i, j in gaps():
        a, b = i, j
        while a < b and text[a] in PADDING:
            a += 1
        while b > a and text[b - 1] in PADDING:
            b -= 1
        if a >= b:
            continue
        # zero runs are padding between functions too
        if not any(text[a:b]):
            continue
        # absolute code pointers inside the data (jump tables kept in .text). The scan starts at the beginning of the gap, before
        # its padding is taken off: the first entry of a table can begin with a byte that looks like padding (0x004e91cc starts
        # with 0xcc), and that entry is a case of the switch like the others
        first_entry = a
        for r in range(4):
            run = []
            for o in range(i + (r - tva - i) % 4, b - 3, 4):
                v = int.from_bytes(text[o:o + 4], "little")
                if v in insn_starts:
                    run.append((tva + o, v))
                    continue
                if len(run) >= 2 or (len(run) == 1 and r == (-tva) % 4):
                    gap_relocs.update(run)
                    first_entry = min(first_entry, run[0][0] - tva)
                run = []
            if len(run) >= 2 or (len(run) == 1 and r == (-tva) % 4):
                gap_relocs.update(run)
                first_entry = min(first_entry, run[0][0] - tva)
        a = first_entry
        ignored.append((tva + a, b - a))

    # ---- data relocations ----
    data_relocs = dict(data_ptrs)
    for name, (va0, vsize, data) in sections.items():
        if name in (".text", ".rsrc", ".sxdata"):
            continue
        tl = textlike_dwords(data)
        for o in range(0, len(data) - 3, 4):
            va = va0 + o
            v = int.from_bytes(data[o:o + 4], "little")
            if not in_image(v) or skipped(va) or in_text(v) or in_text_run(tl, o // 4):
                continue
            data_relocs[va] = v

    relocs = {}
    relocs.update(data_relocs)
    relocs.update(gap_relocs)
    relocs.update(code_relocs)
    # a pointer into .text must hit an instruction or data embedded in the code (jump tables, constants) that is emitted
    ignored_starts = [a for a, _ in ignored]

    def in_ignored(v):
        i = bisect.bisect_right(ignored_starts, v) - 1
        return i >= 0 and v < ignored[i][0] + ignored[i][1]

    dropped = [va for va, v in relocs.items() if in_text(v) and v not in insn_starts and not in_ignored(v)]
    for va in dropped:
        del relocs[va]
    for d in (data_relocs, gap_relocs, code_relocs):
        for va in dropped:
            d.pop(va, None)

    for va, disp_off in fs_sites:
        at = pe.get_offset_from_rva(va - base)
        assert image[at] == 0x64, "fs: site %x is not a 0x64 prefix byte (decoded twice or at another alignment?)" % va
        image[at] = 0x90
        d = pe.get_offset_from_rva(va - base + disp_off)
        image[d:d + 4] = seh_head.to_bytes(4, "little")

    if os.environ.get("PREPARE_DEBUG"):
        lo, hi = (int(x, 16) for x in os.environ["PREPARE_DEBUG"].split("-"))
        print("covered %x-%x:" % (lo, hi), "".join(str(covered[v - tva]) for v in range(lo, hi)), file=sys.stderr)
        print("starts:", [hex(v) for v in range(lo, hi) if v in insn_starts], file=sys.stderr)
        print("roots:", [hex(v) for v in sorted(seen_roots) if lo <= v < hi], file=sys.stderr)
        for v in sorted(insn_starts):
            if lo <= v < hi:
                i = next(md.disasm(text[v - tva:v - tva + 16], v, 1), None)
                print("  %x %s %s%s" % (v, i.mnemonic, i.op_str, "   <- root" if v in seen_roots else "") if i else "  %x ?" % v, file=sys.stderr)
    if os.environ.get("PREPARE_MNEMONICS"):
        with open(os.environ["PREPARE_MNEMONICS"], "w") as f:
            for m, n in sorted(mnemonics.items()):
                f.write("%s %d\n" % (m, n))
    overlaps = []
    prev_end = 0
    for va in sorted(insn_starts):
        if va < prev_end:
            overlaps.append(va)
        prev_end = max(prev_end, va + sizes.get(va, 2))
    if os.environ.get("PREPARE_OVERLAPS"):
        # who decoded the instruction that starts inside another one, and who decoded the one it starts inside
        kinds = collections.Counter()
        owner = {}
        for v in sorted(insn_starts):
            owner[v] = v
        by_start = sorted(insn_starts)
        for va in overlaps:
            k = bisect.bisect_left(by_start, va) - 1
            while k >= 0 and by_start[k] + sizes.get(by_start[k], 2) <= va:
                k -= 1
            outer = by_start[k] if k >= 0 else None
            a, b = chain_root.get(va), chain_root.get(outer)
            kinds[(root_kind.get(a, '?'), root_kind.get(b, '?'))] += 1
            print("  overlap %x (root %s %s) inside %s (root %s %s)" % (va, hex(a) if a else a, root_kind.get(a), hex(outer) if outer else outer, hex(b) if b else b, root_kind.get(b)), file=sys.stderr)
        print("overlap kinds (inner root, outer root):", dict(kinds), file=sys.stderr)
    print("instructions starting inside another instruction: %d %s" % (len(overlaps), [hex(v) for v in overlaps[:10]]), file=sys.stderr)
    if os.environ.get("PREPARE_SITES"):
        for site in (int(x, 16) for x in os.environ["PREPARE_SITES"].split(",")):
            print("=== site %x ===" % site, file=sys.stderr)
            print("  branches/calls to it: %s" % ", ".join("%x %s" % x for x in branch_sources.get(site, [])), file=sys.stderr)
            print("  data/code pointers to it: %s" % ", ".join("%x" % va for va, v in relocs.items() if v == site), file=sys.stderr)
            near = [v for v in sorted(insn_starts) if site - 48 <= v <= site + 20]
            for v in near:
                i = next(md.disasm(text[v - tva:v - tva + 16], v, 1), None)
                print("  %s%x  %s %s" % ("=>" if v == site else "  ", v, i.mnemonic, i.op_str) if i else "  %x ?" % v, file=sys.stderr)
    print("undecodable in reached code:", undecodable.most_common(12), file=sys.stderr)
    # a pointer into the middle of another pointer cannot be real (and llasm has no syntax for it in data)
    interior = [va for va, v in relocs.items() if any((v - k) in relocs and 0 < k < 4 for k in (1, 2, 3))]
    for va in interior:
        del relocs[va]
    for d in (data_relocs, gap_relocs, code_relocs):
        for va in interior:
            d.pop(va, None)
    # The recompiled program keeps the image of the executable at its own addresses, so pointers to data need no
    # relocation and stay what they are in the code; only pointers to code have to become procedures.
    with open(out_relocs, "w") as f:
        for va in sorted(relocs):
            if in_text(relocs[va]) and not in_ignored(relocs[va]):
                f.write("0x%x,0x%x,\n" % (va, relocs[va]))
    # the pointers to code kept in data (tables, vtables, callbacks): the image of the program needs a procedure there
    with open("image_patches.csv", "w") as f:
        patches = {}
        for source in (data_relocs, gap_relocs):
            for va, v in source.items():
                if in_text(v) and not in_ignored(v):
                    patches[va] = v
        for va in sorted(patches):
            f.write("0x%x,0x%x\n" % (va, patches[va]))
    open(out_exe, "wb").write(image)
    with open(out_code_fixups, "w") as f:
        for t in sorted({v for va, v in relocs.items() if in_text(va) and in_text(v)}):
            f.write("loc_%X\n" % t)
    with open(out_replacements, "w") as f:
        for va in sorted(replacements):
            size, text = replacements[va]
            f.write("loc_%X,%d,%s\n" % (va, size, text))
    with open(out_ignored, "w") as f:
        for va, size in ignored:
            f.write("loc_%X,%d\n" % (va, size))
    print("code: %d instructions, %d KB of %d KB covered; relocs: %d (code %d, data %d, in-text tables %d); "
          "fs:[0] patched: %d; ignored areas: %d (%d bytes); dropped text pointers: %d; replaced instructions: %d" % (
              len(insn_starts), sum(covered) // 1024, tsize // 1024, len(relocs), len(code_relocs), len(data_relocs),
              len(gap_relocs), len(fs_sites), len(ignored), sum(s for _, s in ignored), len(dropped), len(replacements)), file=sys.stderr)


if __name__ == "__main__":
    main(*sys.argv[1:7])
