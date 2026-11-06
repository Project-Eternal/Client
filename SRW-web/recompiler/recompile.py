#!/usr/bin/env python3
"""Recompile a Windows executable into the llasm modules the build compiles.

usage: recompile.py PROGRAM.exe SRW.exe OUTPUT_DIR PROGRAM_DIR TRACE_PROCS VA_BASE VA_SIZE

PROGRAM_DIR is the program's own description of the executable: prepare_exe.py, SR.cfg and llasm/*.sci. VA_BASE and VA_SIZE give the
range of the addresses of its code.

Runs, in a scratch directory in PROGRAM_DIR:
  prepare_exe.py   relocations, data-in-code, SEH patch, code fixups        (needs capstone + pefile)
  SRW.exe          the static recompiler (with SRW_COLLECT=1, see below)
  split_llasm.py   data module, CHUNKS code modules, glue
and copies the results to OUTPUT_DIR, leaving files whose content did not change untouched so that the build does not
recompile them.

SRW_COLLECT=1 makes SRW carry on past instructions it cannot translate and list them at the end ("Unsupported: N x ..."). They are
not skipped: the generated code calls SRW_Unsupported_c at that place, which stops the program with the instruction's address if
it is ever reached. What remains at the moment is code that is not taken (3DNow! paths: cpuid does not report 3DNow!, and the
CMOV family is not reported either).
"""
import filecmp
import glob
import os
import shutil
import subprocess
import sys

CHUNKS = 16  # keep in sync with SConstruct

HERE = os.path.dirname(os.path.abspath(__file__))   # (this folder: split_llasm.py)


def run(cmd, cwd, env=None, ok=(0,)):
    result = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    if result.returncode not in ok:
        sys.stderr.write(result.stdout)
        raise SystemExit("%s failed with status %d" % (os.path.basename(cmd[1] if cmd[0] == sys.executable else cmd[0]), result.returncode))
    return result.stdout


def main(exe, srw, outdir, program_dir, trace_procs, va_base, va_size):
    import pefile

    work = os.path.join(program_dir, "work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    pe = pefile.PE(exe, fast_load=True)
    entry = pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.AddressOfEntryPoint

    shutil.copy(srw, os.path.join(work, "SRW.exe"))
    shutil.copy(os.path.join(program_dir, "SR.cfg"), work)
    for sci in glob.glob(os.path.join(program_dir, "llasm", "*.sci")):
        shutil.copy(sci, work)

    out = run([sys.executable, os.path.join(program_dir, "prepare_exe.py"), exe, "relocations.csv", "program.exe", "data_areas.sci",
               "instruction_replacements.sci", "fixup_interpret_as_code.sci"], work)
    sys.stderr.write(out)

    env = dict(os.environ)
    env["SRW_COLLECT"] = "1"
    # SRW crashes while freeing its data structures after it has written everything, hence the status is not checked;
    # the messages tell whether it finished
    out = run([os.path.join(work, "SRW.exe"), "program.exe", "program.llasm"], work, env=env, ok=(0, 6, 8, 139, -11))
    errors = [l for l in out.splitlines() if l.startswith("Error")]
    if errors or "Finishing..." not in out:
        sys.stderr.write(out)
        raise SystemExit("SRW failed: " + (errors[0] if errors else "did not finish"))
    with open(os.path.join(work, "srw.log"), "w") as f:
        f.write(out)
    if os.environ.get("SRW_KEEP_LOG"):   # debugging the recompiler: keep SRW's messages
        with open(os.environ["SRW_KEEP_LOG"], "w") as f:
            f.write(out)

    split_dir = os.path.join(work, "split")
    sys.stderr.write(run([sys.executable, os.path.join(HERE, "split_llasm.py"), work, split_dir, str(CHUNKS), hex(entry), "program.exe", trace_procs, va_base, va_size, program_dir], work))

    os.makedirs(outdir, exist_ok=True)
    produced = sorted(os.listdir(split_dir))
    for name in produced:
        if name == "chunks.txt":
            continue
        src = os.path.join(split_dir, name)
        dst = os.path.join(outdir, name)
        if os.path.isfile(dst) and filecmp.cmp(src, dst, shallow=False):
            continue
        shutil.copyfile(src, dst)
    if not os.environ.get("SRW_KEEP_WORK"):   # debugging the recompiler: SRW can be run again in the work directory
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main(*sys.argv[1:8])
