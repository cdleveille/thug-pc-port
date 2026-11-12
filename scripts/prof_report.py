#!/usr/bin/env python3
"""Name the addresses in a THUG_PROF report (src/host/prof_sampler.h).

Usage: prof_report.py <prof.txt> <thug.exe> [--top N] [--thread TID]

Reads the executable's symbols with nm (llvm-nm or GNU nm on PATH), turns
each thug.exe+0x... into function+offset, and prints per thread, busiest
first: CPU time, then the hottest functions by samples where the thread was
in that function itself (self) and anywhere below it on the stack
(inclusive). Addresses in other modules (the GL driver, ntdll) stay
module+offset, grouped per module for the self view.
"""
import bisect
import collections
import re
import shutil
import subprocess
import sys


def pe_image_base(exe):
    """The ImageBase the executable's symbols are relative to (its PE header
    on disk; the loader rewrites the copy in memory)."""
    import struct
    with open(exe, "rb") as f:
        data = f.read(4096)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    if magic == 0x20B:                       # PE32+
        return struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    return struct.unpack_from("<I", data, pe + 24 + 28)[0]


def load_symbols(exe):
    nm = shutil.which("llvm-nm") or shutil.which("nm")
    if not nm:
        sys.exit("no nm on PATH")
    out = subprocess.run([nm, "-n", "--defined-only", exe], capture_output=True,
                         text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in "TtWw":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def lines_for(prof, exe, func, base, addrs, names, top):
    """--lines FUNC: the hottest source lines inside one function, from the
    self samples at its addresses (llvm-addr2line reads the debug info)."""
    a2l = shutil.which("llvm-addr2line") or shutil.which("addr2line")
    hits = collections.Counter()
    for line in open(prof, encoding="utf-8", errors="replace"):
        m = re.match(r"self \d+ (\d+) thug\.exe\+0x([0-9a-f]+)", line, re.I)
        if not m:
            continue
        a = base + int(m.group(2), 16)
        i = bisect.bisect_right(addrs, a) - 1
        if i >= 0 and names[i] == func:
            hits[a] += int(m.group(1))
    if not hits:
        print(f"no self samples in {func}")
        return
    order = sorted(hits)
    # one line per address: the innermost source line (inlined code included)
    out = subprocess.run([a2l, "-e", exe] + [hex(a) for a in order],
                         capture_output=True, text=True).stdout.split("\n")
    per_line = collections.Counter()
    out_iter = iter(out)
    for a in order:
        loc = next(out_iter, "?")
        per_line[re.sub(r".*[/\\]", "", loc)] += hits[a]
    total = sum(hits.values())
    print(f"{func}: {total} self samples")
    for loc, n in per_line.most_common(top):
        print(f"  {100.0 * n / total:5.1f}%  {loc}")


def main():
    args = sys.argv[1:]
    top = 25
    only = None
    lines = None
    if "--lines" in args:
        i = args.index("--lines")
        lines = args[i + 1]
        del args[i:i + 2]
    if "--top" in args:
        i = args.index("--top")
        top = int(args[i + 1])
        del args[i:i + 2]
    if "--thread" in args:
        i = args.index("--thread")
        only = args[i + 1]
        del args[i:i + 2]
    prof, exe = args
    addrs, names = load_symbols(exe)
    base = pe_image_base(exe)
    if lines:
        lines_for(prof, exe, lines, base, addrs, names, top)
        return
    threads = {}
    self_c = collections.defaultdict(collections.Counter)
    incl_c = collections.defaultdict(collections.Counter)
    header = []
    for line in open(prof, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("exe "):
            continue
        if line.startswith("window"):
            header.append(line)
            continue
        m = re.match(r'thread (\d+) cpu_ms (\S+) cpu_pct (\S+) samples (\d+) name "(.*)"', line)
        if m:
            threads[m.group(1)] = (float(m.group(2)), float(m.group(3)), int(m.group(4)), m.group(5))
            continue
        m = re.match(r"(self|incl) (\d+) (\d+) (\S+)\+0x([0-9a-f]+)(?: (\S+))?", line)
        if not m:
            continue
        kind, tid, n, mod, off = m.group(1), m.group(2), int(m.group(3)), m.group(4), int(m.group(5), 16)
        if mod.lower() == "thug.exe":
            a = base + off
            i = bisect.bisect_right(addrs, a) - 1
            name = names[i] if i >= 0 else "?"
        else:
            name = m.group(6) or mod          # module!export when DbgHelp named it
        (self_c if kind == "self" else incl_c)[tid][name] += n
    print("\n".join(header))
    for tid, (cpu_ms, pct, samples, name) in sorted(threads.items(), key=lambda kv: -kv[1][0]):
        if only and tid != only:
            continue
        if cpu_ms < 50 and not only:
            continue
        print(f"\n== thread {tid} {name!r}: cpu {cpu_ms:.0f} ms ({pct:.1f}%), {samples} samples")
        tot = samples or 1
        print("  self:")
        for fn, n in self_c[tid].most_common(top):
            print(f"    {100.0 * n / tot:5.1f}%  {fn}")
        print("  inclusive:")
        for fn, n in incl_c[tid].most_common(top):
            print(f"    {100.0 * n / tot:5.1f}%  {fn}")


if __name__ == "__main__":
    main()
