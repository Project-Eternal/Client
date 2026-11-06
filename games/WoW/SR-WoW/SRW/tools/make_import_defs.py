#!/usr/bin/env python3
"""Write imports.def: the signature of every function WoW.exe imports.

usage: make_import_defs.py WoW.exe SPEC_DIR > ../imports.def

SPEC_DIR holds the Wine `.spec` files (dlls/<name>/<name>.spec from the Wine repository); they are only needed to
regenerate the table, the build reads the committed result. Each line:

    <name in the llasm> <dll> <argument count> <argument kinds...>

argument kinds: i (32-bit value), p (pointer), s (ANSI string), w (wide string), q (64-bit value, two slots),
f (32-bit float), d (double, two slots); a count of -1 means the signature is not known.
The name in the llasm is the import name as SRW writes it: stdcall decoration removed, and the winsock functions that are
imported by ordinal carry the `ws2_32.` prefix where SRW puts one.
"""
import os
import re
import sys

import pefile

KIND = {"word": "i", "long": "i", "ptr": "p", "str": "s", "wstr": "w", "int64": "q", "float": "f", "double": "d",
        "segptr": "p", "segstr": "s", "int128": "q"}
SLOTS = {"i": 1, "p": 1, "s": 1, "w": 1, "f": 1, "q": 2, "d": 2}

DLL_SPECS = {"kernel32": ["kernel32", "kernelbase"], "advapi32": ["advapi32", "kernelbase"], "user32": ["user32"],
             "gdi32": ["gdi32"], "comctl32": ["comctl32"], "shell32": ["shell32"], "imm32": ["imm32"],
             "wsock32": ["ws2_32"], "ws2_32": ["ws2_32"], "winmm": ["winmm"], "wininet": ["wininet"],
             "opengl32": ["opengl32"]}


def load_specs(specdir):
    specs = {}
    line_re = re.compile(r"^(?:@|\d+)\s+(stdcall|cdecl|varargs|pascal|thiscall)\s+((?:-\S+\s+)*)(\w+)\s*\(([^)]*)\)")
    for name in os.listdir(specdir):
        if not name.endswith(".spec"):
            continue
        table = {}
        for line in open(os.path.join(specdir, name), errors="ignore"):
            m = line_re.match(line)
            if not m:
                continue
            conv, flags, func, args = m.groups()
            kinds = [KIND.get(a, "i") for a in args.split()]
            if "-ret64" in flags:
                pass
            table.setdefault(func, (conv, kinds))
        specs[name[:-5]] = table
    return specs


def clean_name(name):
    # what SRW_loader.c does with a decorated name
    if "@" in name:
        name = name.split("@")[0]
        if name.startswith("_"):
            name = name[1:]
    return name


def main(exe, specdir):
    specs = load_specs(specdir)
    pe = pefile.PE(exe)
    seen = set()
    rows = []
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode().lower()
        base = dll.rsplit(".", 1)[0]
        for imp in entry.imports:
            if imp.name is None:
                continue
            raw = imp.name.decode()
            name = clean_name(raw)
            if base == "wsock32":
                # SRW writes the ordinal imports of wsock32 as ws2_32.<name> (a few without the prefix)
                pass
            key = name
            if key in seen:
                continue
            seen.add(key)
            nargs, kinds = -1, []
            for spec in DLL_SPECS.get(base, []):
                if name in specs.get(spec, {}):
                    conv, kinds = specs[spec][name]
                    nargs = sum(SLOTS[k] for k in kinds)
                    break
            else:
                m = re.search(r"@(\d+)$", raw)
                if m:
                    nargs = int(m.group(1)) // 4
                    kinds = ["i"] * nargs
            rows.append((name, base, nargs, kinds))
    for name, dll, nargs, kinds in sorted(rows, key=lambda r: (r[1], r[0])):
        print("%s %s %d %s" % (name, dll, nargs, " ".join(kinds)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
