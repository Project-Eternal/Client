#!/usr/bin/env python3
"""Split the llasm SRW produces for the program into modules that can be assembled and compiled one by one.

The whole program is ~4.8 million lines of llasm and ~275 MB of LLVM IR, which does not fit into memory in one piece.
Input (in the directory given):  program.llasm  seg01_code.llinc  seg0N_data.llinc
Output (in the out directory):   data.ll                data segments, exports the labels code uses (`dlabel ... global`)
                                 code-NN.llasm           chunks of consecutive procedures
                                 chunks.txt             list of the generated .llasm files
Procedures used from another module become `public` where they are defined and `external` where they are used.

                                 imports.txt            the imports, and entry.txt the entry procedure, for gen_glue.py
                                 vamap.c                the map from the addresses of the code to its procedures

usage: split_llasm.py INPUT_DIR OUTPUT_DIR CHUNKS ENTRY_ADDRESS EXE TRACE_PROCS VA_BASE VA_SIZE PROGRAM_DIR

The data of the program is not translated: the runtime keeps the image of the executable at its own addresses
(data.ll, one global at 0x400000), so that pointers to data need no relocation. What the recompiled program
still needs to patch into the image are the pointers to code and to imported functions, found in the data output of SRW.

With TRACE_PROCS = 1 every procedure starts by calling SRW_Proc_c with its address (a ring buffer in the runtime keeps the
last ones), for finding out where the program went.
"""
import os
import re
import sys

IDENT = re.compile(r"\b(loc_[0-9A-Fa-f]+|imagebase1000)\b")
# a token in the code that could name a procedure or a data label that is not a loc_ one
EXTRA_IDENT = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")


def read(path):
    with open(path, "r", newline="\n") as f:
        return f.read()


IMAGE_BASE = 0x400000


def image_ir(exe, patches, path):
    """the executable's image at its addresses as an LLVM module with the pointer patches as constants.

It is declared `constant` only so that the linker puts it in the first data segment (read-only data comes before the rest);
linear memory is writable, and nothing compiled assumes it does not change."""
    import pefile

    pe = pefile.PE(exe)
    end = max(s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData) for s in pe.sections)
    end = (end + 0xfff) & ~0xfff
    image = bytearray(end)
    image[:len(pe.header)] = pe.header
    for sec in pe.sections:
        data = sec.get_data()
        image[sec.VirtualAddress:sec.VirtualAddress + len(data)] = data

    patch_at = {}
    for addr, sym in patches:
        patch_at[addr - IMAGE_BASE] = sym
    symbols = sorted(set(patch_at.values()))
    offsets = sorted(patch_at)
    for a, b in zip(offsets, offsets[1:]):
        if b < a + 4:
            raise SystemExit("overlapping pointers at 0x%x and 0x%x" % (IMAGE_BASE + a, IMAGE_BASE + b))

    def escape(chunk):
        out = []
        for b in chunk:
            if b == 0x22 or b == 0x5c or b < 0x20 or b >= 0x7f:
                out.append("\\%02X" % b)
            else:
                out.append(chr(b))
        return "".join(out)

    types, values = [], []

    def raw(a, b):
        i = a
        while i < b:
            if image[i] == 0:
                j = i
                while j < b and image[j] == 0:
                    j += 1
                if j - i >= 16:
                    types.append("[%d x i8]" % (j - i))
                    values.append("[%d x i8] zeroinitializer" % (j - i))
                    i = j
                    continue
            # a run of data up to the next long stretch of zeros (or 4 KB)
            j = i
            while j < b and j - i < 4096:
                if image[j] == 0:
                    k = j
                    while k < b and image[k] == 0:
                        k += 1
                    if k - j >= 16:
                        break
                    j = k
                else:
                    j += 1
            types.append("[%d x i8]" % (j - i))
            values.append('[%d x i8] c"%s"' % (j - i, escape(image[i:j])))
            i = j

    pos = 0
    for off in offsets:
        raw(pos, off)
        types.append("i32")
        values.append("i32 ptrtoint (i8* (%%_cpu*)* @%s to i32)" % patch_at[off])
        pos = off + 4
    raw(pos, len(image))

    with open(path, "w") as f:
        f.write('target datalayout = "e-p:32:32-f64:32:64-n32"\n\n%_cpu = type opaque\n\n')
        for sym in symbols:
            f.write("declare fastcc i8* @%s(%%_cpu*)\n" % sym)
        f.write("\n@srw_image = constant <{ %s }> <{\n  %s\n}>, align 4096\n" % (", ".join(types), ",\n  ".join(values)))
    return len(image)


def main(indir, outdir, chunk_count, entry, exe, trace_procs, va_base, va_size, program_dir):
    os.makedirs(outdir, exist_ok=True)
    main_text = read(os.path.join(indir, "program.llasm"))
    code_text = read(os.path.join(indir, "seg01_code.llinc"))

    # ---- the top-level file: header, imports, data segments
    header, imports, segments = [], [], []
    for line in main_text.split("\n"):
        m = re.match(r"proc (\S+) external\s*$", line)
        if m:
            imports.append(m.group(1))
        elif line.startswith("include seg01_code.llinc"):
            pass
        elif line.startswith("include ") and "llinc" in line and not line.startswith("include seg"):
            header.append(line)
    # the imports of the program have names of C library functions too (send, select, glClear...): they get a prefix
    def renamed(name):
        return "srw_" + name

    rename = {n: renamed(n) for n in imports if renamed(n) != n}
    imports = [renamed(n) for n in imports]
    rename_rx = re.compile(r"\b(" + "|".join(re.escape(n) for n in sorted(rename, key=len, reverse=True)) + r")\b") if rename else None
    import_set = set(imports)

    # ---- data segments: which labels exist, which procedures the data points at
    data_labels = set()
    data_text_parts = []
    seg_re = re.compile(r"^(datasegment .*|endd|include (seg\d+_data\.llinc)|dlabel imagebase1000)\s*$")
    seg_lines = [l for l in main_text.split("\n") if seg_re.match(l)]
    for line in seg_lines:
        m = re.match(r"include (seg\d+_data\.llinc)", line)
        if m:
            body = read(os.path.join(indir, m.group(1)))
            for lm in re.finditer(r"^dlabel (\S+)", body, re.M):
                data_labels.add(lm.group(1))
            data_text_parts.append(("body", m.group(1), body))
        else:
            if line.startswith("dlabel "):
                data_labels.add(line.split()[1])
            data_text_parts.append(("line", None, line))

    # ---- code: split into procedure blocks
    if rename_rx is not None:
        code_text = "\n".join(l if l.lstrip().startswith(";") else rename_rx.sub(lambda m: rename[m.group(1)], l) for l in code_text.split("\n"))
    # Code that falls into a table kept in the code (behind a call that does not return) or pushes the address of one as the
    # return address refers to a data label: that is the address of the data in the image, which is where the program has it
    def data_address(line):
        body = line.split(";", 1)[0]
        if not any(m.group(1) in data_labels for m in IDENT.finditer(body)):
            return [line]
        stripped = body.strip()
        m = re.match(r"tcall (loc_[0-9A-Fa-f]+)$", stripped)
        if m and m.group(1) in data_labels:
            return ["mov tmp1, 0x" + m.group(1)[4:], "tcall tmp1"]
        if stripped.upper().startswith(("PUSH ", "MOV ")):
            return [IDENT.sub(lambda k: "0x" + k.group(1)[4:] if k.group(1) in data_labels else k.group(1), body)]
        return [line]

    code_text = "\n".join(out for line in code_text.split("\n") for out in data_address(line))
    # the functions the program exports by name (e.g. AssertAndCrash) are SRW's procedures with that name: they are plain code addresses
    import pefile as _pefile
    _pe = _pefile.PE(exe, fast_load=True)
    _pe.parse_data_directories([_pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    for sym in getattr(getattr(_pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", []):
        if sym.name:
            n = sym.name.decode()
            code_text = re.sub(r"^proc %s global c_%s$" % (re.escape(n), re.escape(n)), "proc %s" % n, code_text, flags=re.M)
            code_text = re.sub(r"\b%s\b" % re.escape(n), "loc_%X" % (_pe.OPTIONAL_HEADER.ImageBase + sym.address), code_text)
    # procedures replaced by hand-written bodies that call native code (native_procs.llasm, see there)
    native_path = os.path.join(program_dir, "native_procs.llasm")
    if os.path.isfile(native_path):
        # two directives in comment lines before a procedure: ";;keep loc_X" keeps the body it replaces under that name (an address in the
        # padding after the original, so that native code can call it with SRW_CallGuest and the stub can continue in it);
        # ";;add" is a procedure that replaces nothing (it is added)
        native, keep, added, current, pending_keep, pending_add = {}, {}, [], None, None, False
        for line in open(native_path).read().split("\n"):
            if line.startswith(";;"):
                if line.startswith(";;keep "):
                    pending_keep = line.split()[1]
                elif line.strip() == ";;add":
                    pending_add = True
                continue
            if line.startswith("proc "):
                name = line.split()[1]
                if pending_add:
                    added.append(name)
                if pending_keep is not None:
                    keep[name] = pending_keep
                pending_keep, pending_add = None, False
                current = native.setdefault(name, [])
            if current is not None:
                current.append(line)
            if line == "endp":
                current = None
        out, skipping, replaced, original = [], False, 0, []
        for line in code_text.split("\n"):
            if skipping:
                original.append(line)
                if line == "endp" or line.startswith("endp "):
                    skipping = False
                    if keep.get(original_name) is not None:
                        original[0] = "proc " + keep[original_name]
                        out.extend(original)
                continue
            if line.startswith("proc ") and line.split()[1] in native and line.split()[1] not in added:
                original_name = line.split()[1]
                original = [line]
                out.extend(native[original_name])
                skipping = True
                replaced += 1
                continue
            out.append(line)
        for name in added:
            out.extend(native[name])
        if replaced != len(native) - len(added):
            raise SystemExit("native_procs.llasm: %d of %d procedures were not found" % (len(native) - len(added) - replaced, len(native) - len(added)))
        code_text = "\n".join(out)
    lines = code_text.split("\n")
    blocks = []  # (name, first line index, last line index)
    cur = None
    for i, line in enumerate(lines):
        if line.startswith("proc "):
            if cur is not None:
                raise SystemExit("proc inside proc at line %d" % (i + 1))
            cur = [line.split()[1], i]
        elif line == "endp" or line.startswith("endp "):
            if cur is None:
                raise SystemExit("endp without proc at line %d" % (i + 1))
            blocks.append((cur[0], cur[1], i))
            cur = None
    if cur is not None:
        raise SystemExit("unterminated proc " + cur[0])
    proc_names = {b[0] for b in blocks}

    # verify nothing but blank lines / comments lies between the blocks
    prev_end = -1
    for name, a, b in blocks:
        for j in range(prev_end + 1, a):
            t = lines[j].strip()
            if t and not t.startswith(";"):
                raise SystemExit("stray line outside a procedure: %d: %s" % (j + 1, lines[j]))
        prev_end = b

    # exactly chunk_count chunks, so that the build can name its outputs in advance
    per_chunk = -(-len(blocks) // chunk_count)
    chunk_of = {}
    chunks = []
    for k in range(chunk_count):
        chunk = blocks[k * per_chunk:(k + 1) * per_chunk]
        chunks.append(chunk)
        for name, a, b in chunk:
            chunk_of[name] = k
    entry_proc = "loc_%X" % entry
    if entry_proc not in proc_names:
        raise SystemExit("the entry point %s is not a procedure" % entry_proc)

    # ---- who uses what
    used_by_code = [set() for _ in chunks]
    for ci, chunk in enumerate(chunks):
        for name, a, b in chunk:
            for j in range(a, b + 1):
                for m in IDENT.finditer(lines[j].split(";", 1)[0]):
                    used_by_code[ci].add(m.group(1))

    patches = []
    have = set()
    # the import address table: what the program reads from it is the address of the procedure of the import
    import pefile

    pe = pefile.PE(exe)
    missing = []
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        for imp in entry.imports:
            name = imp.name.decode() if imp.name else None
            if name and name.startswith("_") and "@" in name:
                name = name[1:name.rindex("@")]  # _Name@8 -> Name
            if name in rename:
                name = rename[name]
            if name not in import_set and ("ws2_32." + str(name)) in rename:
                name = rename["ws2_32." + name]  # SRW names the socket functions after their newer library (WSOCK32 is imported by ordinal)
            if name in import_set:
                if imp.address not in have:
                    patches.append((imp.address, name))
            else:
                missing.append("%s:%s" % (entry.dll.decode(), name or imp.ordinal))
    if missing:
        print("note: imports without a procedure: %s" % ", ".join(missing[:10]), file=sys.stderr)
    public = {entry_proc}
    for ci, used in enumerate(used_by_code):
        for n in used:
            if n in proc_names and chunk_of[n] != ci:
                public.add(n)
    exported_data = set()
    for used in used_by_code:
        for n in used:
            if n in data_labels:
                exported_data.add(n)
    if exported_data:
        raise SystemExit("the code refers to data labels (it should use the addresses): %s" % ", ".join(sorted(exported_data)[:8]))

    # ---- data module: the image of the executable, with the pointers to code
    image_size = image_ir(exe, patches, os.path.join(outdir, "data.ll"))
    written = ["data.ll"]

    # ---- code chunks
    for ci, chunk in enumerate(chunks):
        if not chunk:
            chunk = []
        defined = {b[0] for b in chunk}
        out = []
        out.extend(header)
        out.append("")
        for n in imports:
            out.append("proc %s external" % n)
        needs_proc = sorted(n for n in used_by_code[ci] if n in proc_names and n not in defined)
        for n in needs_proc:
            out.append("proc %s external" % n)
        out.append("")
        for name, a, b in chunk:
            if name in public:
                lines[a] = "proc %s public" % name
            if trace_procs:
                lines[a] += "\nmov tmp0, 0x%s\ncall SRW_Proc_c tmp0" % name[4:]
        if chunk:
            out.extend(lines[chunk[0][1]:chunk[-1][2] + 1])
        # the procedures of this chunk with their addresses, for the table SRW_VaInit builds (vamap.c)
        out.append("")
        out.append("datasegment .vamap constant")
        out.append("dlabel srw_va_procs_%02d global" % ci)
        for name, a, b in chunk:
            out.append("daddr %s" % name)
        out.append("dlabel srw_va_addrs_%02d global" % ci)
        for name, a, b in chunk:
            va = int(name[4:], 16)
            out.extend("db %d" % ((va >> shift) & 0xff) for shift in (0, 8, 16, 24))
        out.append("endd")
        fname = "code-%02d.llasm" % ci
        with open(os.path.join(outdir, fname), "w") as f:
            f.write("\n".join(out) + "\n")
        written.append(fname)

    # the table that turns the addresses of the program's code into procedures
    with open(os.path.join(outdir, "vamap.c"), "w") as f:
        f.write("/* generated by split_llasm.py: the map from the addresses of the program's code to its procedures */\n")
        f.write("#include <stdint.h>\n\n")
        f.write("#define SRW_VA_BASE 0x%xu\n#define SRW_VA_SIZE 0x%xu\n\n" % (va_base, va_size) + "")
        f.write("uint32_t SRW_VaMap[SRW_VA_SIZE];\nconst uint32_t SRW_VaBase = SRW_VA_BASE, SRW_VaSize = SRW_VA_SIZE;\nconst uint32_t SRW_ImageSize = 0x%xu;\n\n" % image_size)
        for ci in range(len(chunks)):
            f.write("extern const uint32_t srw_va_procs_%02d[], srw_va_addrs_%02d[];\n" % (ci, ci))
        f.write("\nstatic const uint32_t *const procs[] = {%s};\n" % ", ".join("srw_va_procs_%02d" % ci for ci in range(len(chunks))))
        f.write("static const uint32_t *const addrs[] = {%s};\n" % ", ".join("srw_va_addrs_%02d" % ci for ci in range(len(chunks))))
        f.write("static const uint32_t counts[] = {%s};\n\n" % ", ".join(str(len(c)) for c in chunks))
        f.write("void SRW_VaInit(void)\n{\n    unsigned int k, i;\n\n    for (k = 0; k < %d; k++)\n    {\n" % len(chunks))
        f.write("        for (i = 0; i < counts[k]; i++)\n        {\n            uint32_t offset = addrs[k][i] - SRW_VA_BASE;\n\n")
        f.write("            if (offset < SRW_VA_SIZE) SRW_VaMap[offset] = procs[k][i];\n        }\n    }\n}\n")
    written.append("vamap.c")

    # what gen_glue.py needs to build the glue
    with open(os.path.join(outdir, "imports.txt"), "w") as f:
        f.write("\n".join(imports) + "\n")
    with open(os.path.join(outdir, "entry.txt"), "w") as f:
        f.write(entry_proc + "\n")
    written += ["imports.txt", "entry.txt"]

    with open(os.path.join(outdir, "chunks.txt"), "w") as f:
        f.write("\n".join(written) + "\n")
    print("procedures: %d in %d chunks; %d public, %d imports; image of %d KB with %d code pointers" % (
        len(blocks), len(chunks), len(public), len(imports), image_size // 1024, len(patches)), file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4], 0), sys.argv[5], sys.argv[6] == "1", int(sys.argv[7], 0), int(sys.argv[8], 0), sys.argv[9])
